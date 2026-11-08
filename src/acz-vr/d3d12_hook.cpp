// Submission observation is incomplete: discovery here misses first recording,
// and ExecuteCommandLists is not one display frame.
#include "d3d12_hook.h"
#include "native_provenance_evidence.h"
#include "capture_requirements_proof.h"
#include "lifetime_diagnostics.h"
#include "root_constant_witness.h"
#include "l3_observation.h"
#include "srv_candidate_ring.h"
#include "ownership_shutdown.h"
#include "gpu_copy_ledger.h"
#include "gpu_copy_observation.h"
#include "gpu_copy_observation_slots.h"
#include "gpu_copy_owner.h"
#include "gpu_copy_step1.h"
#include "gpu_copy_step2.h"
#include "gpu_copy_step3.h"
#include "gpu_copy_hud_observe.h"
#include "gpu_copy_step5.h"         // J-3b8 (must precede the anonymous namespace)
#include "gpu_copy_step5_world.h"   // J-world-objects (gpu_copy_step5_world_objects, default OFF)
#include "gpu_copy_png.h"           // J-hud-colour png_once (default OFF)
#include "eye_ring.h"               // EYE-RING: per-eye producer targets at the eye size (eyeres > 0 + eyeobj; must precede the anonymous namespace)
#include "terrain_camera.h"          // WORLD-1: the frame-synchronous E6 sample (gpu_copy_step5_hooks.inl, frame_cam_mailbox)
#include "gpu_copy_precockpit_observe.h" // J-cockpit-fov F2 (default OFF)
#include "gpu_copy_world_observe.h"      // J-world-objects v2 plan step 1 (gpu_copy_step5_world_observe, default OFF)
#include "gpu_copy_world_evidence.h"     // J-world-evidence (Astra reply 5; runs under the world_observe / world_objects keys)
#include "gpu_copy_phase_observe.h"      // J-phase-observe (gpu_copy_step5_phase_observe, default OFF)
#include "stereo_pose_provider.h"
#include "feature_log.h"
#ifndef ACZ_VR_OFFLINE_TEST
#include "xr_stereo_output.h" // consumer rev 5: register_producer_fence
#endif
#include "ee_camera_hooks.h"        // J-w1-ee-camera seams (default OFF: null atomics)
#include "hud_msl.h"                // MSL-HUD (mslhud, default on since step172; mslhud:0 = off): HUD MSSL count -> acz::stores::signal()
#include "store_tracker.h"          // acz::stores::signal() (must precede the anonymous namespace: one mailbox per module)
#include "look_drive_keys.h"        // xr_look_drive_keys (default OFF; header-only logic, the real injector is look_drive_keys.cpp)
#include "upload_creation_pins.h"
#include "perf_counters.h"        // FPS investigation: summary-producer-cost counters (observe only)
#ifndef ACZ_VR_OFFLINE_TEST
#include "vscreen_capture.h"      // VSCREEN: swap chain -> queue association (Astra finding 3)
#include "launch_mode_win.h"      // LAUNCH-MODE: VR On/Off gate of the factory exports and the early-device notification
#endif
#include <windows.h>
#include "fx_puff_atlas_d3d12.h"    // EFFECTS-OUTSIDE (M6) fxatlas: the capture pipeline / atlas descriptions (shared with the WARP harness)
#if defined(ACZ_FX_ATLAS_SHADERS)
#include "fx_puff_atlas_vs.h"       // g_fx_atlas_vs (fxc, fx_puff_atlas.hlsl vs_copy; acz-vr target only)
#include "fx_puff_atlas_ps.h"       // g_fx_atlas_ps (fxc, fx_puff_atlas.hlsl ps_copy)
#endif
#include <bcrypt.h>
#include <d3d12sdklayers.h>
#include <d3d12compatibility.h>
#include <d3d11on12.h>
#include <atomic>
#include <climits>
#include <memory>
#include <optional>
#include <new>
#include <string>
#include <unordered_map>
#include <array>
#include "shader_metadata.h"
#include "shader_dump.h"            // EFFECTS-OUTSIDE (B): dump-shaders.txt read-only diagnostic (absent file: inert)
#include "briefing_observe.h"       // BRIEFING-3D: briefing-observe.arm draw histogram (absent file: one relaxed load per draw)
#include "ui3d_census.h"             // UI3D: ui3d-census.arm read-only menu draw census (absent file: one relaxed load per draw)
#include "brief3d_capture.h"         // BRIEF3D: brief3d-capture.arm read-only map-draw capture (absent file: one relaxed load per draw)
#include "brief3d_live.h"            // BRIEF3D live (brief3d:1): per-eye replay of the briefing map draws (off: one relaxed load per hook)
#include <mutex>
#include <type_traits>
#include <stdexcept>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <string_view>
#include <limits>

namespace acz {
namespace {
struct ListState {
    std::shared_ptr<gpu_copy::ObservationFeed> copyObservation; // null while OFF
    std::shared_ptr<ID3D12PipelineState> pso;
    std::shared_ptr<ID3D12RootSignature> root;
    std::vector<std::shared_ptr<ID3D12DescriptorHeap>> heaps;
    capture::State bindings;
    diagnostics::RootConstantWitness constantWitness;
    diagnostics::RootDescriptorTableWitness tableWitness, samplerWitness;
    bool capture_hooks=false;
    bool native_binding_history=false;
    unsigned registration_path=3; // 0 base, 1 reserved (D-86), 2 Reset, 3 unobserved
    bool creation_observed=false, reset_observed=false, reset_before_capture_hooks=false, interfaces_observed=false;
    // Per-object aliases were published and every final-Release seam hooked.
    // Shared table coverage alone does not establish routing for this object.
    bool cleanup_established=false;
    const char* observation_gap=nullptr;
    uint64_t native_binding_device=0;
    void** table=nullptr;
    std::array<void**,15> tables{};
    // J-3b7: RS/OM state of the current recording, written only by the step-3
    // tracking hooks (installed only while gpu_copy_step3_experiment is on).
    // `known`: the recording began at an observed Reset/ClearState while every
    // graphics variant table of this list had those hooks installed, so the
    // D3D12 default state (nothing bound) is the starting point. `gap`: a setter
    // call whose arguments could not be recorded; the state is then unknown.
    struct Step3Track {
        bool known=false,gap=false;
        UINT viewports=0,scissors=0;D3D12_VIEWPORT vp0{};D3D12_RECT sc0{};
        float right=0,bottom=0,left=0,top=0;
        bool omSet=false;UINT omCount=0;BOOL omSingle=FALSE;
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE,8> rtv{};bool dsv=false;D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle{};
        // J-3b9 (review P2): bind-time descriptor proof. The write serial of every bound
        // RTV/DSV handle (step3Views) and the RTV/DSV copy epoch at OMSetRenderTargets;
        // the restore is valid only while none of them changed (else refused).
        std::array<uint64_t,8> rtvSerial{};uint64_t dsvSerial=0,copyEpoch=0;bool omProof=false;
        // J-3b9c (review 2, P2): PRIVATE copies of the bound descriptors' contents, taken at bind
        // time into the proxy's OM ring; the restore binds these (valid even if the game later
        // overwrites or frees its own descriptor heap). privSeq ages them out of the ring.
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE,8> privRtv{};D3D12_CPU_DESCRIPTOR_HANDLE privDsv{};uint64_t privSeq=0;bool privOk=false;
        // J-3b9 world-snapshot barrier facts for the watched game RT in this generation.
        bool rtTransition=false,rtBad=false,enhanced=false;uint32_t rtAfter=0;const char* rtWhy=nullptr;
        uintptr_t rtResource=0;uint64_t rtWatchEpoch=0,rtSerial=0,rtGeneration=0;
        bool rtPendingCounted=false; // J-3b9c: this generation touched the watched RT and is not yet executed
    } step3;
};
using ListStates=std::unordered_map<ID3D12GraphicsCommandList*,ListState>;
struct Hooks {
    // Set only by metadata lifetime installation, never inferred from map membership.
    bool metadataRelease=false;
    std::array<void*, 86> originals{};
    std::array<DWORD, 86> pendingProtection{};
    // Diagnostic state only, serialized by the existing hook mutex.
    std::array<unsigned char, 86> outcomes{};
};
// Sixteen permanent reservations, including partial/faulted attempts. Orderly cleanup below.
using SrvCandidates=diagnostics::SrvSlotCandidates<ID3D12Resource>;
struct OwnershipPin {
    SrvCandidates::Key textureKey{};
    ID3D12Resource* buffer=nullptr; // legacy/test-owned pins; Draw never acquires buffers
    binding::Token buffer_token{};
    uint64_t buffer_serial=0;
    ID3D12Resource* texture=nullptr; // own-interface permanent reference
    unsigned textureReason=0;
    ID3D12PipelineState* base=nullptr;
    capture::Id identity=0,rootIdentity=0;
    ID3D12RootSignature* rootKey=nullptr;
    ULONG rootCount=0; // positive return of the existing root retain
    std::shared_ptr<ID3D12RootSignature> root; // shares the existing retain
    bool rootCanonicalOwned=false; // successful nonnull QI, even if provenance later refuses
    IUnknown* rootCanonical=nullptr; // owned only when rootCanonicalOwned is set
    std::array<IUnknown*,8> interfaces{};
    std::array<ULONG,8> counts{};
    bool reserved=false,complete=false;
};
// Diagnostic only: one bit/count per refused creation, not per failed conjunct.
// Counts saturate; existing logger quotas still bound emitted records.
struct PinRefusals {
    enum Reason : unsigned { hash=1, shape=2, validation=4, seams=8,
        split_root=16, provenance=32, creation_interface=64, cap=128,
        switch_off=256, reference=512, native_return=1024, observation=2048,
        root_qi=4096 };
    unsigned sticky=0;
    unsigned qiAttempts=0,qiSuccesses=0;
    std::array<unsigned,12> counts{};
    void record(unsigned flags) noexcept {
        sticky|=flags&0xfff; // Preserve the original three-digit field and twelve counts.
        for(size_t i=0;i<counts.size();++i)if((flags&(1u<<i))&&counts[i]<65535)++counts[i];
    }
    void append(char* text,size_t capacity,unsigned flags) const noexcept {
        auto n=std::strlen(text);
        if(n>=capacity)return;
        std::snprintf(text+n,capacity-n,";pf=%03x;ps=%03x;pn=",flags&0xfff,sticky);
        for(auto count:counts) {
            n=std::strlen(text);if(n>=capacity)return;
            std::snprintf(text+n,capacity-n,"%04x",count);
        }
        n=std::strlen(text);if(n>=capacity)return;
        std::snprintf(text+n,capacity-n,";rq=%u;ra=%04x;rs=%04x",
            unsigned(bool(flags&root_qi)),qiAttempts,qiSuccesses);
    }
};
struct PipelineRecord {
    diagnostics::LifetimeConditions conditions,dependency_conditions;
    std::array<char,208> detail{}; // formatted once at creation
    std::array<uint64_t,20> retirementKeys{};
    bool retirementKnown=false; // immutable, complete creation footprint
    unsigned ownershipPin=0; // one-based fixed slot; zero uses observation only
    capture::Id ownershipIdentity=0; // pin acquisition token; creation identity stays fresh
    bool ownershipRequired=false; // deferred release used the ownership proof
    capture::Id publication=0; // diagnostic ordering key only, never serialized
    metadata::ShaderMetadata shader;
    std::shared_ptr<ID3D12RootSignature> root;
    // J-effects-dual: the private alpha-blend clone of a PSO whose blend is exactly the game's dual-source tuple (created with it while
    // gpu_copy_step5_effects_dual is on; released with the record, i.e. with the original PSO, which the game keeps alive for the GPU).
    std::shared_ptr<ID3D12PipelineState> effectsDualClone;
    // EFFECTS-OUTSIDE (I) fxmem:6: the explosion tuple's SECOND private PSO (alpha SRC1_ALPHA / SRC_ALPHA / REV_SUBTRACT: the game's (1+a) background
    // brightening as transmittance), used ONLY by the per-eye sprite replay; same lifetime as effectsDualClone. Null = the shared clone is used (fail safe).
    std::shared_ptr<ID3D12PipelineState> effectsExplosionLight;
    // EFFECTS-OUTSIDE (M6) fxatlas: the 4caf6e2d dual tuple's capture pipeline (the game's root signature + fx_puff_atlas.hlsl) and the device that made it;
    // same lifetime as effectsDualClone. Null without the generated shaders (offline builds) or when its creation failed.
    std::shared_ptr<ID3D12PipelineState> effectsAtlasCopy;uintptr_t effectsAtlasDevice=0;
    // HUD-LATE display marker (hud_late.h, review F6): the HUD text pipeline's private clone with alpha ONE / ONE / REV_SUBTRACT (created with hudlate:1 only);
    // replayed instead of the game's PSO for a late text draw while the consumer's tone map / exposure are not the defaults. Same lifetime as the others.
    std::shared_ptr<ID3D12PipelineState> hudLateClone;
    capture::Id identity=0, root_identity=0;
    std::array<char,33> hudVs{},hudPs{}; // J-171: existing creation hashes, no rehash at Draw
    uint32_t brVs8=0,brPs8=0; // BRIEFING-3D: first 8 hex digits of the VS/PS creation hashes, always recorded (briefing-observe histogram key)
    bool drawcpt_identity_matched=false;
    bool drawcpt_ia_layout=false;
    // J-world-objects: creation-time DrawACM_v identity (VS HASH + full container SHA-256, PS partial fingerprint,
    // no GS/HS/DS/SO, the four-float4 input layout). Set only with gpu_copy_step5_world_objects; inert otherwise.
    bool acm_world_matched=false;
    uint32_t vs_cb10_words=0,ps_cb10_words=0; // creation-authenticated E11 facts
    uint32_t strip_cut=index_range::unknown_cut,topology_type=0;
    gpu_copy::step3::PsoOutput output; // J-3b7: creation-time OM/depth facts of the desc
    // J-phase-observe: render target 0 blend factors/operations as created (D3D12_BLEND / D3D12_BLEND_OP numerics) and the back face
    // stencil pass op. Read-only creation facts, kept here (not in PsoOutput: the Draw-seam Shape copies that); nothing replays from them.
    uint8_t blend_src=0,blend_dst=0,blend_op=0,blend_src_alpha=0,blend_dst_alpha=0,blend_op_alpha=0,back_pass=0;
    // J-world-evidence: render target 1 as created, EFFECTIVE (with IndependentBlendEnable false D3D12 applies RenderTarget[0] to every
    // target, so slot 1 shows slot 0's values then); the independent flag is output.independent_blend.
    bool blend1=false;uint8_t write_mask1=0;
    // Creation-time facts only; never infer COM shape/eligibility at Draw.
    enum class Canonical { unavailable, self, split };
    Canonical canonical=Canonical::unavailable;
    bool creation_interface_supported=false;
    // Non-owning, validated creation-time IUnknown key. A held QI reference
    // pins publication; final Release through any validated key retires this token.
    IUnknown* canonical_address=nullptr;
    std::array<IUnknown*,6> ancestor_addresses{}; // validated, covered Release seams
};
// FPS growth fix (docs/world-render/fps-investigation.md section 7): HookState::pipelines only grows while the game keeps creating PSOs
// (its pipeline cache is never released), and two lookups walked EVERY record whenever the address was not a direct key:
//  * pipelineFor_locked's alias search (canonical_address / ancestor_addresses), ~12-15 times per list on EVERY ExecuteCommandLists
//    (hookCaptureCommandList -> metadataOwner) and once per hooked Release of any root/heap/resource (releaseMetadata, including the
//    old binding each SetGraphicsRootSignature / SetDescriptorHeaps releases inside the setter);
//  * registeredOwnershipKey_locked's map-key / retirementKeys search (Release of an unregistered object on a hooked table).
// This index counts, per address, the records that claim it. "No record claims it" is answered in O(1); any claim (or an index that
// cannot be trusted) still runs the ORIGINAL scan, so every returned record and every boolean is unchanged.
// Section 8 (residual pscan): a CLAIMED address still ran the scan. Each address now also carries the XOR of the map keys of the records
// claiming it, and every record claims an address at most once, so `count` = number of claiming records and, when count == 1, `keys` IS
// the sole claimant's key: pipelineFor_locked returns that record directly (the scan's "first claimant in iteration order" is then the
// only claimant) and registeredOwnershipKey_locked answers from `count` alone. Only addresses claimed by 2+ records still scan.
struct PipelineKeyIndex {
    struct Claim {unsigned count=0;uintptr_t keys=0;}; // claiming records; XOR of their map keys (== the key while count == 1)
    std::unordered_map<uintptr_t,Claim> aliases;    // canonical_address + ancestor_addresses (pipelineFor_locked)
    std::unordered_map<uintptr_t,Claim> retirement; // map key + retirementKeys (registeredOwnershipKey_locked)
    uint64_t epoch=UINT64_MAX;size_t size=SIZE_MAX;bool valid=false;
    uint64_t rebuilds=0;
    static void claim(std::unordered_map<uintptr_t,Claim>& m,uintptr_t address,uintptr_t key,bool add) {
        if(!address)return;
        if(add){auto& c=m[address];++c.count;c.keys^=key;return;}
        auto it=m.find(address);if(it==m.end())return;
        it->second.keys^=key;if(!--it->second.count)m.erase(it);
    }
    // Duplicates inside one record are claimed once (the same dedupe on add and on remove, so the counts stay exact).
    template<size_t N> static void claimAll(std::unordered_map<uintptr_t,Claim>& m,const std::array<uintptr_t,N>& in,uintptr_t key,bool add) {
        for(size_t i=0;i<N;++i)if(in[i]&&std::find(in.begin(),in.begin()+i,in[i])==in.begin()+i)claim(m,in[i],key,add);
    }
    void apply(uintptr_t key,const PipelineRecord& r,bool add) {
        std::array<uintptr_t,7> a{};a[0]=reinterpret_cast<uintptr_t>(r.canonical_address);
        for(size_t i=0;i<r.ancestor_addresses.size();++i)a[1+i]=reinterpret_cast<uintptr_t>(r.ancestor_addresses[i]);
        claimAll(aliases,a,key,add);
        std::array<uintptr_t,21> k{};k[0]=key;
        for(size_t i=0;i<r.retirementKeys.size();++i)k[1+i]=uintptr_t(r.retirementKeys[i]);
        claimAll(retirement,k,key,add);
    }
    void clear() noexcept {aliases.clear();retirement.clear();valid=false;}
};
struct RootCreationRecord {
    std::array<uint64_t,20> retirementKeys{};
    bool retirementKnown=false;
    diagnostics::LifetimeConditions conditions;
    std::vector<uint8_t> bytes;
    ID3D12Device* device=nullptr;
    UINT node=0;
    IID iid{};
    ID3D12RootSignature* interfaceAddress=nullptr;
    capture::Id identity=0, publication=0;
};
// Diagnostic historical raw-address sightings, never identity/lifetime proof.
// No eviction: after capacity loss an absent key is explicitly unknown.
struct CreationSightings {
    enum Unavailable : unsigned { capacity_lost=1, snapshot_unspecified=2,
        clock_mismatch=4, other=8 };
    enum Seam : unsigned { root=1, pso=2, list=4, descriptor_heap=8,
        committed=16, placed=32, reserved=64, compatibility_resource=128,
        compatibility_heap=256 };
    struct Entry { uintptr_t address=0; unsigned seams=0; };
    static constexpr size_t capacity=4096;
    std::array<Entry,capacity> entries{};
    unsigned occupancy=0, highWater=0;
    // No eviction policy: this remains zero, including after capacity loss.
    unsigned evictions=0;
    bool overflow=false;
    // FPS hook cost (docs/world-render/fps-investigation.md section 8): classify() ran on every hooked Release of an unregistered object
    // (every list Release, e.g. the ~30 QI outputs certification drops per list per Execute, and every untracked resource/heap) and walked
    // all 4096 entries (64 KB) each time. `index` is an open-addressing hash over `entries` (entry position + 1, 0 = empty slot, load
    // <= 1/2). Entries are append-only (no eviction, never removed one by one), so the first empty entry is always entries[filled] and the
    // index answers exactly what the original walks answered. The arrays are members, so whole-struct copies/assignments (fixtures, the
    // texture-hook save/restore) keep entries and index together. If the append-only shape is ever not intact (a direct fixture edit of
    // `entries`), the original walks run instead.
    static constexpr size_t index_slots=2*capacity;
    static_assert((index_slots&(index_slots-1))==0&&capacity<UINT16_MAX);
    std::array<uint16_t,index_slots> index{};
    unsigned filled=0;
    static size_t slot(uintptr_t address) noexcept {
        uint64_t x=uint64_t(address);x^=x>>33;x*=0xff51afd7ed558ccdULL;x^=x>>33;return size_t(x)&(index_slots-1);
    }
    bool indexed() const noexcept {
        return filled<=capacity&&(filled==capacity||!entries[filled].address)&&(!filled||entries[filled-1].address);
    }
    // nullptr: absent. Caller checked indexed().
    const Entry* lookup(uintptr_t address) const noexcept {
        for(size_t i=slot(address),probes=0;probes<index_slots;i=(i+1)&(index_slots-1),++probes) {
            const auto at=index[i];if(!at)return nullptr;
            if(entries[at-1u].address==address)return &entries[at-1u];
        }
        return nullptr;
    }
    const Entry* walk(uintptr_t address) const noexcept {
        for(const auto& e:entries)if(e.address==address)return &e;
        return nullptr;
    }
    const Entry* find(uintptr_t address) const noexcept {
        if(!indexed())return walk(address);
        const auto* hit=lookup(address);
#ifdef ACZ_VR_OFFLINE_TEST
        if(hit!=walk(address)){++mismatches;return walk(address);} // test builds: every answer equals the original walk
#endif
        return hit;
    }
#ifdef ACZ_VR_OFFLINE_TEST
    mutable uint64_t mismatches=0;
#endif
    void observe(uintptr_t address,unsigned seam) noexcept {
        if(!address)return;
        if(const auto* hit=find(address)) {const_cast<Entry*>(hit)->seams|=seam;return;}
        if(indexed()) {
            if(filled<capacity) {
                entries[filled]={address,seam};
                size_t i=slot(address);while(index[i])i=(i+1)&(index_slots-1); // load <= 1/2: an empty slot exists
                index[i]=uint16_t(filled+1u);++filled;
                ++occupancy;highWater=(std::max)(highWater,occupancy);return;
            }
        } else for(auto& e:entries)if(!e.address) { // original path (append-only shape not intact; index unused from here on)
            e={address,seam};++occupancy;highWater=(std::max)(highWater,occupancy);return;
        }
        overflow=true;
    }
    // 0 seen raw address, 1 absent from complete history, 2 unknown history.
    unsigned classify(uintptr_t address) const noexcept {
        if(!address)return 2;
        if(find(address))return 0;
        return overflow?2u:1u;
    }
};
// Same begin/end clock as the binding cohort. Addresses are opaque comparison
// keys only; include both interfaces and canonical keys and PSO dependencies.
enum RetirementRefusal : unsigned {
    child_incomplete=1u<<0, unregistered=1u<<1, unsupported_identity=1u<<2,
    binding_owner=1u<<3, native_activity=1u<<4, certificate_overflow=1u<<5,
    parent_key=1u<<6, retained_key=1u<<7, child_ambiguity=1u<<8,
    unknown_peer=1u<<9, missing_history=1u<<10, clock_serial=1u<<11,
    parent_incomplete=1u<<12
};
struct MetadataFootprint {
    // 15 QI outputs, raw/output addresses and two dependency keys (19 max).
    // Preserve malformed outputs too; overflow still refuses ordering.
    std::array<uint64_t,20> objects{};
    uint64_t begin=0,end=0;
    DWORD thread=0;
    bool known=false,overflow=false;
    bool ownershipCandidate=false; // pre-native absence of all registered/pinned keys
    bool releaseComplete=false; // certified pre-native snapshot, not identify()
    unsigned releaseRefusals=0; // immutable diagnostic provenance, never grants proof
    unsigned creationSighting=2; // snapshot before native Release; diagnostic only
    unsigned sightingUnavailable=CreationSightings::snapshot_unspecified;
    uint64_t sightingClock=0; // same cohort clock, not an object generation
    const char* kind="release";
};
struct MetadataEvent {
    MetadataEvent* next=nullptr;
    IUnknown* address=nullptr;
    capture::Id serial=0;
    bool creation=false, isolated=false, ambiguous=false;
    // Sticky writer evidence, independent of the eligibility flag. No keys or
    // thread IDs are serialized. Counts saturate at 255 rather than wrap.
    enum Ambiguity : unsigned { reentry=1, active_same=2, active_other=4,
        completed_same=8, completed_other=16, capacity=32,
        binding_reentry=64, binding_key_same=128, binding_key_other=256 };
    unsigned ambiguityReasons=0, ambiguityKinds=0;
    unsigned nestedReleases=0; // append-only audit, including refused children
    unsigned provenNestedReleases=0; // unresolved decisions never grant distinct()
    unsigned nestedRefusals=0, passedChildren=0;
    std::array<unsigned,13> nestedReasonCounts{};
    std::array<unsigned,3> unregisteredSightings{};
    std::array<unsigned,4> unavailableSightings{};
    bool ownershipPinned=false; // set only from a complete creating-thread pin
    unsigned ownershipChildren=0; // children accepted only by ownership
    bool nestedResolved=false; // one append-only resolution snapshot
    std::array<unsigned,4> ambiguityHigh{};
    MetadataFootprint footprint;
    mutable const char* blocking="none";
    // Reuse the 182 static pair/relation literals; one extra root-only fallback
    // covers missing history/self footprints. No new disable-reason slots.
    mutable const char* overlap="overlap:unresolved";
    MetadataEvent(IUnknown*,bool,const char* kind=nullptr);
    MetadataEvent(const MetadataEvent&)=delete;
    MetadataEvent& operator=(const MetadataEvent&)=delete;
    ~MetadataEvent();
    void identify(IUnknown*); // caller holds the mutex
    void touch(IUnknown*); // caller holds mutex; does not replace earlier keys
    bool distinct(const struct HookState&) const;
    diagnostics::LifetimeConditions orderConditions(const struct HookState&) const;
    std::array<unsigned,4> overlapCounts() const noexcept; // mutex held
    void mark(unsigned reason,const MetadataFootprint* other=nullptr) noexcept;
    void appendAmbiguity(char*,size_t) const noexcept; // creation snapshot only
    void resolveNestedReleases() noexcept; // mutex held, before PSO publication
    void tag(const MetadataFootprint& other) const noexcept {
        using K=binding::ObservationKind;
        const auto kind=std::string_view(other.kind);
        overlap=binding::overlap_reason(K::root_create,
            // Generic metadata Release can retire a root or PSO; do not claim
            // it is the more specific tracked buffer resource_release kind.
            kind=="root_create"?K::root_create:kind=="pso_create"?K::pso_create:
            kind=="release"?K::metadata_release:K::other,
            footprint.thread==other.thread);
    }
    void tag(const struct BindingRecord& other) const noexcept;
};
struct HookState {
    diagnostics::Mutex mutex;
    // Permanent observation reservations; never recycle a possibly retained
    // list lifetime. Weak slots hold no native owner and bound refusal work.
    std::array<std::weak_ptr<gpu_copy::ObservationFeed>,64> copyFeeds{};
    unsigned copyFeedReservations=0,copyFeedUnavailable=0;
    // Entire observeRequirements_locked interval holds mutex, with no native
    // dispatch or unlock. Same-thread native hook nesting happens outside it.
    struct RequirementsScratch {
        diagnostics::RootConstantWitness::Snapshot constants;
        diagnostics::DescriptorTableDiagnostics::Snapshot table,sampler;
        requirements::MetadataResult result;
        char context[272]{},diagnostic[diagnostics::text_capacity]{};
    } requirementsScratch;
    diagnostics::CpuUploadSnapshot cpuSnapshot;
    // J-3b7b (step 3 only): last 128 CreateRenderTargetView/CreateDepthStencilView
    // destinations -> resource address, so the game's bound RTV can be described.
    // Diagnostic only (descriptor copies and pre-hook views are not seen).
    struct Step3ViewDesc {bool present=false;uint32_t format=0,dim=0,mip=0,slice=0;}; // J-world-evidence: the game's RTV description
    struct Step3View {SIZE_T handle=0;uintptr_t resource=0;bool depth=false,described=false;
        uint64_t width=0;uint32_t height=0,format=0,heap=0;
        uint32_t samples=0,flags=0,dim=0;bool sub0=false;uint64_t serial=0; // J-3b9: desc + the view names subresource 0 + write serial
        Step3ViewDesc vd{};};
    uint64_t step3ViewSerial=0,step3CopyEpoch=0; // J-3b9 descriptor-write evidence (RTV/DSV)
    // J-3b9c world RT entry state carried across EXECUTED lists (end state of the last executed
    // generation that touched it), and the count of recorded-but-unexecuted generations that did.
    bool step5RtCarriedKnown=false;uint32_t step5RtCarried=0;int step5RtPending=0;
    uint64_t step5WatchEpoch=0,step5RtSerial=0;uintptr_t step5RtResource=0;ID3D12CommandQueue* step5RtQueue=nullptr;
    struct WorldLifetime {void** table=nullptr;uint64_t serial=0;};
    std::unordered_map<uintptr_t,WorldLifetime> step5WorldLifetimes;
    uint64_t step5WatchSerial=0,step5WatchClock=0; // J-155 fix: watched world RT incarnation (0 = unknown/retired)
    // J-3b9c: proxy-owned, non-shader-visible RTV/DSV heaps (owner context) holding bind-time
    // copies of the game's OM descriptors: a ring of `binds` slots (8 RTV + 1 DSV each).
    struct Step3OmRing {bool ready=false;ID3D12Device* device=nullptr;void* copy=nullptr;SIZE_T rtvBase=0,dsvBase=0;UINT rtvInc=0,dsvInc=0;uint64_t seq=0;
        ID3D12DescriptorHeap* rtvHeap=nullptr;ID3D12DescriptorHeap* dsvHeap=nullptr;static constexpr unsigned binds=256;
        std::array<unsigned,binds> pins{};std::array<uint64_t,binds> versions{};} step3Om;
    std::array<Step3View,128> step3Views{};unsigned step3ViewNext=0;
    diagnostics::UploadCreationPins<ID3D12Resource> uploadPins;
    diagnostics::L3Observation l3; // process heap only, never an automatic local
    std::unordered_map<void**, Hooks> lists, queues;
    ListStates states;
    // Non-owning aliases, all certified against QI(IUnknown). Retired together.
    std::unordered_map<void*, ID3D12GraphicsCommandList*> listKeys;
    std::unordered_map<ID3D12PipelineState*, bool> gs;
    std::unordered_map<void**, Hooks> devices, objects;
    std::unordered_map<void**, Hooks> compatibility;
    // At most one canonical-table category example per calling seam for the
    // process. Uses the existing device/startup quota; no new logger partition.
    std::array<bool,3> canonicalTableReported{};
    struct CompatibilityOwner {ID3D12Device* device;capture::Id token;};
    std::unordered_map<IUnknown*,CompatibilityOwner> compatibilityOwners;
    std::unordered_map<ID3D12PipelineState*, PipelineRecord> pipelines;
    // FPS growth fix: claims index over `pipelines` (pipelineIndex_locked / pipelineIndexEdit_locked). Every production mutation of
    // `pipelines` bumps the epoch; an index whose epoch/size no longer matches is rebuilt before it answers.
    PipelineKeyIndex pipelineIndex;uint64_t pipelineEpoch=0;
    std::unordered_map<ID3D12RootSignature*, metadata::RootBindingMetadata> roots;
    std::unordered_map<ID3D12RootSignature*, capture::Id> rootIdentities;
    std::unordered_map<ID3D12RootSignature*,RootCreationRecord> rootCreations;
    std::array<OwnershipPin,16> ownershipPins{};
    SrvCandidates srvCandidates;
    unsigned pinsTaken=0,pinsCapRefused=0; // saturating diagnostics
    unsigned pinDedupeHits=0,cb10ConstantDraws=0,constantWordCalls=0;
    diagnostics::RootConstantCounters constantWitnessCounts;
    diagnostics::DescriptorTableCounters tableWitnessCounts;
    PinRefusals pinRefusals;
    CreationSightings creationSightings;
    // Non-owning interface aliases; all root maps use the canonical IUnknown
    // address as an opaque key (never dereferenced as a root interface).
    std::unordered_map<IUnknown*, ID3D12RootSignature*> rootKeys;
    MetadataEvent* metadataEvents=nullptr;
    capture::Id rootEventSerial=0;
    unsigned rootCreates=0, metadataReleases=0;
    DeviceReadyCallback deviceCb;
    DrawCallback drawCb;
    ExecuteCallback executeCb;
    BindingCallback bindingCb;
    binding::NativeBindingObserver nativeBindings;
    std::atomic<unsigned> bindingEvents{0};
    uint64_t requirements_last_us=0;
    unsigned requirements_attempts=0;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
};
// Intentional process-lifetime ownership: no COM/hook destruction at CRT detach.
std::atomic<HookState*> allocatedState{nullptr};
diagnostics::OwnershipShutdown ownershipShutdown;
std::atomic<bool> ownershipWasEnabled{false};
HookState& state() {
    static auto* value = [] {auto* p=new HookState;allocatedState.store(p);return p;}();
    return *value;
}
struct HookAdmission {
    bool admitted=false;
    HookAdmission() noexcept {
        admitted=ownershipShutdown.enter();
        if(admitted)if(auto* log=diagnostics::sink();log&&log->ownership_pins())ownershipWasEnabled.store(true);
    }
    ~HookAdmission(){if(admitted)ownershipShutdown.leave();}
    explicit operator bool() const noexcept{return admitted;}
};
void releaseOwned(IUnknown* value) noexcept;
#ifdef ACZ_VR_OFFLINE_TEST
bool g_l3Test=false; // J-3b6 fixture: L3 observation as if diagnostic_ownership_pins were latched
bool g_worldObjectsTest=false; // J-world-objects fixture: gpu_copy_step5_world_objects as if latched
bool g_worldObserveTest=false; // J-world-objects v2 fixture: gpu_copy_step5_world_observe as if latched
bool g_phaseObserveTest=false; // J-phase-observe fixture: gpu_copy_step5_phase_observe as if latched
#endif
// J-world-objects (gpu_copy_step5_world_objects, default OFF; latched only with the step-5 producer and
// members_by_class). Every world-object path starts with this one relaxed load; key off = no extra work.
bool worldObjectsConfigured() noexcept {
#ifdef ACZ_VR_OFFLINE_TEST
    if(g_worldObjectsTest)return true;
#endif
    auto* log=diagnostics::sink();return log&&log->gpu_copy_step5_world_objects();
}
// J-world-objects v2 plan step 1 (gpu_copy_step5_world_observe, default OFF; latched only with the step-5 producer).
// Every observer path starts with this one relaxed load; key off = no extra work.
bool worldObserveConfigured() noexcept {
#ifdef ACZ_VR_OFFLINE_TEST
    if(g_worldObserveTest)return true;
#endif
    auto* log=diagnostics::sink();return log&&log->gpu_copy_step5_world_observe();
}
// J-phase-observe (gpu_copy_step5_phase_observe, default OFF; latched only with the step-5 producer). One relaxed load per path.
bool phaseObserveConfigured() noexcept {
#ifdef ACZ_VR_OFFLINE_TEST
    if(g_phaseObserveTest)return true;
#endif
    auto* log=diagnostics::sink();return log&&log->gpu_copy_step5_phase_observe();
}
// The DrawACM identity is evaluated at PSO creation (and the vertex-visible root SRV t10 accepted) when either the world replay or
// the observer needs it. The replay itself stays behind worldObjectsConfigured() only.
bool worldIdentityConfigured() noexcept {return worldObjectsConfigured()||worldObserveConfigured()||phaseObserveConfigured();}
// J-world-evidence (Astra reply 5): no key of its own; on with either world key. Key off = one relaxed load per path.
bool worldEvidenceConfigured() noexcept {return worldObjectsConfigured()||worldObserveConfigured();}
// PSO-creation sightings of the DrawACM identity (process lifetime; read by the world status line).
// J-180: psFingerprint = the partial 8+4 hex fingerprint matched (diagnostic only); psFull = the decoded hash AND the full container SHA-256
// matched (the acceptance gate); psUnverified = fingerprint matched WITHOUT the full pair (refused; the live hash/digest are logged, bounded).
struct WorldObjectsPsoCounts {std::atomic<unsigned> vsMatch{0},vsHashOnly{0},psFingerprint{0},matched{0},psFull{0},psUnverified{0};};
WorldObjectsPsoCounts g_worldPso;
void worldObjectsPsoSeen(bool vs,bool vsHashOnly,bool ps,bool matched,bool psFull=false,bool psUnverified=false) noexcept {
    if(vs)g_worldPso.vsMatch.fetch_add(1,std::memory_order_relaxed);
    if(vsHashOnly)g_worldPso.vsHashOnly.fetch_add(1,std::memory_order_relaxed);
    if(ps)g_worldPso.psFingerprint.fetch_add(1,std::memory_order_relaxed);
    if(matched)g_worldPso.matched.fetch_add(1,std::memory_order_relaxed);
    if(psFull)g_worldPso.psFull.fetch_add(1,std::memory_order_relaxed);
    if(psUnverified)g_worldPso.psUnverified.fetch_add(1,std::memory_order_relaxed);
}
diagnostics::L3Observation& l3_locked() noexcept {
    auto& o=state().l3;auto* log=diagnostics::sink();o.enabled=log&&log->ownership_pins();
#ifdef ACZ_VR_OFFLINE_TEST
    if(g_l3Test)o.enabled=true;
#endif
    return o;
}
diagnostics::L3Observation::List* l3_list_locked(const ListState& list) noexcept {
    auto& o=l3_locked();if(!o.enabled)return nullptr;
    // capture::State also advances its binding generation on ClearState.
    // That does not erase recorded draws/barriers. L3 advances only on an
    // observed successful native Reset (or a new list lifetime).
    const auto* prior=o.lists.get(list.bindings.list_identity);
    return o.list(list.bindings.list_identity,prior?prior->generation:list.bindings.generation);
}
// What l3ConfigureT10 counted (diagnostic read-out for the world observer; no behaviour).
struct L3T10Outcome {unsigned cockpitMatches=0,worldMatches=0,matches=0;};
void l3ConfigureT10(diagnostics::L3Observation::List& l,const metadata::RootBindingMetadata& root,L3T10Outcome* outcome=nullptr) noexcept {
    l.t10_parameter=UINT32_MAX;l.t10_table=false;l.t10_offset=0;
    l.t10_vs_parameter=UINT32_MAX;l.t10_vs_table=false;l.t10_vs_offset=0;l.vs_source=diagnostics::L3Observation::Source{};
    if(outcome)*outcome=L3T10Outcome{};
    if(!root.valid())return;
    // Two stage rules, evaluated per parameter (root signature deny flags are per stage):
    //  * DrawCPT: the record is read by the geometry shader; an ALL/GEOMETRY-visible t10 qualifies unless DENY_GEOMETRY_SHADER_ROOT_ACCESS (16).
    //  * DrawACM_v (only with world_objects / world_observe / phase_observe): there is no geometry shader; the VERTEX shader reads the record,
    //    so a VERTEX- or ALL-visible t10 qualifies unless DENY_VERTEX_SHADER_ROOT_ACCESS (2). An ALL-visible root SRV in a signature that
    //    denies the geometry stage (flag 16, the usual VS+PS signature) is exactly what the first live fix (vertex visibility only) missed:
    //    live step31 still had t10-va=0 in all 168 ACM draw records.
    const uint32_t rootFlags=root.layout().flags;
    const bool world=worldIdentityConfigured();
    // J-180 (Codex review P2): candidates are tracked PER CONSUMING STAGE. A parameter qualifies for the cockpit (GS) rule, the world (VS) rule, or both;
    // ambiguity is judged inside each stage, so a VERTEX-visible t10 at one parameter and a GEOMETRY-visible t10 at another (disjoint stages, each
    // honouring its own deny flag) are two unambiguous bindings, not one ambiguous one. The combined count (`matches`) is kept for the diagnostics.
    struct Candidate {unsigned parameter=UINT32_MAX,offset=0;bool table=false;};
    unsigned matches=0,cockpitCount=0,worldCount=0;Candidate cockpit,vertex,any;
    const auto& parameters=root.layout().parameters;
    auto note=[&](bool cockpitStage,bool worldStage,const Candidate& c) {
        ++matches;any=c;
        if(cockpitStage){++cockpitCount;cockpit=c;}
        if(worldStage){++worldCount;vertex=c;}
        if(outcome){if(cockpitStage)++outcome->cockpitMatches;else ++outcome->worldMatches;}
    };
    for(unsigned i=0;i<parameters.size()&&i<64;++i) {
        const auto& p=parameters[i];
        const bool cockpitStage=(p.visibility==metadata::Visibility::all||p.visibility==metadata::Visibility::geometry)&&!(rootFlags&16);
        const bool worldStage=world&&(p.visibility==metadata::Visibility::vertex||p.visibility==metadata::Visibility::all)&&!(rootFlags&2);
        if(!cockpitStage&&!worldStage)continue;
        if(p.kind==metadata::ParameterKind::srv&&p.shader_register==10&&!p.space) {
            note(cockpitStage,worldStage,Candidate{i,0,false});
        } else if(p.kind==metadata::ParameterKind::table) {
            uint64_t cursor=0;
            for(unsigned j=0;j<p.ranges.size()&&j<64;++j) {
                const auto& r=p.ranges[j];const uint64_t offset=r.offset==metadata::append_range?cursor:r.offset;
                cursor=offset+r.count;
                if(r.kind!=metadata::BindingKind::srv||r.space||r.base_register>10||10-r.base_register>=r.count)continue;
                const auto selected=offset+10-r.base_register;if(selected>UINT32_MAX)continue;
                note(cockpitStage,worldStage,Candidate{i,static_cast<uint32_t>(selected),true});
            }
        }
    }
    if(outcome)outcome->matches=matches;
    // The general binding (cockpit GS rule; with a world key also the only candidate of either stage, as before): exactly one candidate overall,
    // or exactly one inside the cockpit stage.
    const Candidate* general=matches==1?&any:cockpitCount==1?&cockpit:nullptr;
    if(general){l.t10_parameter=general->parameter;l.t10_offset=general->offset;l.t10_table=general->table;}
    // The vertex-stage binding (only meaningful with a world key: worldStage requires it): exactly one candidate inside the vertex stage.
    if(worldCount==1){l.t10_vs_parameter=vertex.parameter;l.t10_vs_offset=vertex.offset;l.t10_vs_table=vertex.table;}
}
// Formatting scratch is one record wide. No registry/snapshot copies.
void l3_sources_locked(const ListState& list,diagnostics::L3Observation::List& l) noexcept {
    auto& o=state().l3;char text[diagnostics::text_capacity]{};
    for(unsigned i=0;i<l.sources.size();++i) {
        if(i>0&&i<33&&!l.sources[i].va)continue;
        const auto* prefix=i==0?"l3-t10":i==33?"l3-ib":"l3-vb";
        o.facts(text,sizeof text,prefix,l.sources[i],i);
        diagnostics::emit(diagnostics::record(diagnostics::Event::requirements_metadata,list.bindings.list_identity,l.generation,l.draws,text));
        o.state_text(text,sizeof text,l,list.bindings.list_identity,l.generation,l.sources[i],i);
        diagnostics::emit(diagnostics::record(diagnostics::Event::requirements_metadata,list.bindings.list_identity,l.generation,l.draws,text));
    }
}
void observeCreationReturn(HRESULT hr,void** out,unsigned seam) {
    if(FAILED(hr)||!out||!*out)return;
    auto& s=state();std::lock_guard lock(s.mutex);
    s.creationSightings.observe(reinterpret_cast<uintptr_t>(*out),seam);
}
// Caller holds HookState::mutex. Original dispatch always uses the actual self.
ID3D12GraphicsCommandList* listKey_locked(void* self) {
    auto& keys=state().listKeys;auto it=keys.find(self);
    return it==keys.end()?static_cast<ID3D12GraphicsCommandList*>(self):it->second;
}

// Diagnostic interval includes native dispatch and publication. No metadata lock
// survives dispatch. An alias uses the existing canonical list-key lookup.
struct ConstantCall {
    void* self; diagnostics::RootConstantWitness::Call call{};
    bool enabled=false;
    uint64_t bindingRevision=0;
    unsigned descriptorBefore=UINT32_MAX;
    explicit ConstantCall(void* value):self(value) {
        auto* log=diagnostics::sink();if(!log||!log->ownership_pins())return;
        auto& s=state();std::lock_guard lock(s.mutex);
        auto it=s.states.find(listKey_locked(self));if(it==s.states.end())return;
        enabled=true;auto& list=it->second;bindingRevision=list.bindings.revision;
        if(const auto* d=s.nativeBindings.diagnostic();d&&!s.bindingEvents.load())descriptorBefore=d->write_ordinal;
        call=list.constantWitness.begin(list.bindings.list_identity,list.bindings.generation,GetCurrentThreadId());
    }
    void returned() noexcept {
        if(!enabled)return;
        auto& s=state();std::lock_guard lock(s.mutex);
        auto it=s.states.find(listKey_locked(self));if(it==s.states.end())return;
        it->second.constantWitness.returned(call);
    }
    void drawReturned(const capture::Command&) noexcept;
    ~ConstantCall() {
        if(!enabled)return;
        auto& s=state();std::lock_guard lock(s.mutex);
        auto it=s.states.find(listKey_locked(self));if(it==s.states.end())return;
        it->second.constantWitness.end(call);
    }
};
// Resolve only when binding the root, never at Draw. VS and PS are independent.
void configureConstantWitness(ListState& list) noexcept {
    auto* log=diagnostics::sink();if(!log||!log->ownership_pins())return;
    try {
    using namespace metadata;
    const auto& b=list.bindings;
    unsigned index=0,length=0,stages=0;bool accepted=false;
    if(b.root&&b.root->valid()) {
        const auto vs=b.root->resolve(BindingKind::cbv,10,0,ShaderStage::vertex);
        const auto ps=b.root->resolve(BindingKind::cbv,10,0,ShaderStage::pixel);
        const bool v=vs.valid&&vs.parameter_kind==ParameterKind::constants;
        const bool p=ps.valid&&ps.parameter_kind==ParameterKind::constants;
        stages=unsigned(v)|(unsigned(p)<<1);
        index=v?vs.root_index:ps.root_index;
        accepted=(v||p)&&(!v||!p||vs.root_index==ps.root_index);
        if(accepted&&index<b.root->layout().parameters.size())length=b.root->layout().parameters[index].constant_count;
        else accepted=false;
    }
    list.constantWitness.bind(b.list_identity,b.generation,b.root_id,b.root_identity,index,length,stages,accepted);
    } catch(...) {list.constantWitness.fail(diagnostics::RootConstantWitness::history);}
    try {
    using namespace metadata;const auto& b=list.bindings;
    const auto t=b.root&&b.root->valid()?b.root->resolve(BindingKind::srv,0,0,ShaderStage::pixel):BindingResolution{};
    list.tableWitness.bind(b.root_id,b.root_identity,t.root_index,t.table_offset,
        t.valid&&t.parameter_kind==ParameterKind::table);
    const auto sampler=b.root&&b.root->valid()?b.root->resolve(BindingKind::sampler,0,0,ShaderStage::pixel):BindingResolution{};
    list.samplerWitness.bind(b.root_id,b.root_identity,sampler.root_index,sampler.table_offset,
        sampler.valid&&sampler.parameter_kind==ParameterKind::table);
    } catch(...) {list.tableWitness.configured=false;list.samplerWitness.configured=false;}
}

#ifdef ACZ_VR_OFFLINE_TEST
// fps_growth_offline: every index answer is cross-checked against the original scan in test builds; a disagreement is counted here
// and the scan's answer is used (so fixtures that edit `pipelines` in place keep their exact pre-index behaviour).
uint64_t g_pipelineIndexMismatch=0;
#endif
// Caller holds the mutex. nullptr = no trustworthy index (allocation failure): the caller runs the original scan.
const PipelineKeyIndex* pipelineIndex_locked() noexcept {
    auto& s=state();auto& x=s.pipelineIndex;
    if(x.valid&&x.epoch==s.pipelineEpoch&&x.size==s.pipelines.size())return &x;
    perf::Scope cost(perf::pso_scan); // a rebuild walks every record once (counted with the remaining scans)
    try {
        x.clear();
        for(const auto& e:s.pipelines)x.apply(reinterpret_cast<uintptr_t>(e.first),e.second,true);
        x.valid=true;x.epoch=s.pipelineEpoch;x.size=s.pipelines.size();++x.rebuilds;
        perf::gauge(perf::pipelines,s.pipelines.size());
        return &x;
    } catch(...) {x.clear();return nullptr;}
}
// Caller holds the mutex and has ALREADY applied the map edit: `removed` = the record that left key `key` (nullptr: none), `added` = the
// record now stored at `key` (nullptr: none). An index that was not current before this edit (a fixture edited the map directly, or an
// allocation failed) is left invalid and rebuilt by the next lookup.
void pipelineIndexEdit_locked(uintptr_t key,const PipelineRecord* removed,const PipelineRecord* added) noexcept {
    auto& s=state();auto& x=s.pipelineIndex;
    const size_t before=x.size;const uint64_t epoch=x.epoch;
    ++s.pipelineEpoch;perf::gauge(perf::pipelines,s.pipelines.size());
    if(!x.valid||epoch!=s.pipelineEpoch-1)return;
    const size_t expected=before+(added&&!removed?1:0)-(removed&&!added?1:0);
    if(s.pipelines.size()!=expected){x.clear();return;}
    try {
        if(removed)x.apply(key,*removed,false);
        if(added)x.apply(key,*added,true);
        x.epoch=s.pipelineEpoch;x.size=s.pipelines.size();
    } catch(...) {x.clear();}
}
// The original whole-map alias search (kept verbatim; the index only skips it when no record claims the address).
auto pipelineScan_locked(IUnknown* object) {
    auto& records=state().pipelines;
    perf::Scope cost(perf::pso_scan);
    return std::find_if(records.begin(),records.end(),[&](const auto& entry) {
        return entry.second.canonical_address==object ||
            std::find(entry.second.ancestor_addresses.begin(),entry.second.ancestor_addresses.end(),object)!=
                entry.second.ancestor_addresses.end();
    });
}
// Caller holds the mutex. Table membership is never per-object ownership.
// Keep the alias in the record so publication/removal cannot leave a partial
// secondary map behind on allocation failure.
auto pipelineFor_locked(IUnknown* object) {
    auto& records=state().pipelines;
    if(!object)return records.end();
    auto direct=records.find(reinterpret_cast<ID3D12PipelineState*>(object));
    if(direct!=records.end())return direct;
    if(const auto* x=pipelineIndex_locked()) {
        const auto claim=x->aliases.find(reinterpret_cast<uintptr_t>(object));
        // FPS growth fix: no record claims this address -> the scan below cannot match (O(1) instead of O(records)).
        if(claim==x->aliases.end()) {
#ifdef ACZ_VR_OFFLINE_TEST
            const auto scan=pipelineScan_locked(object);
            if(scan!=records.end()){++g_pipelineIndexMismatch;return scan;}
#endif
            return records.end();
        }
        // Section 8: exactly one record claims it -> that record is the scan's only possible answer. Confirmed against the record itself
        // (any doubt falls through to the scan). Typical caller: releaseMetadata for a Release through a split-identity PSO's canonical /
        // ancestor address (not the map key), e.g. the runtime's own references; previously a scan to that record every time.
        if(claim->second.count==1) {
            const auto unique=records.find(reinterpret_cast<ID3D12PipelineState*>(claim->second.keys));
            if(unique!=records.end()&&(unique->second.canonical_address==object||
               std::find(unique->second.ancestor_addresses.begin(),unique->second.ancestor_addresses.end(),object)!=
                   unique->second.ancestor_addresses.end())) {
#ifdef ACZ_VR_OFFLINE_TEST
                const auto scan=pipelineScan_locked(object);
                if(scan!=unique){++g_pipelineIndexMismatch;return scan;}
#endif
                return unique;
            }
        }
    }
    return pipelineScan_locked(object);
}

// Intrusive, allocation-free active observations. Unknown native outputs block
// fresh publication. Known same-address overlap taints BOTH observations even
// if one finishes first. A returned COM reference pins the observed lifetime;
// canonical QI references are held until publication and event retirement.
// A bounded cohort retains completed footprints until every overlapping event
// retires. Creation outputs are unknown before native return. Compare their
// addresses when identified,
// including against already completed retirements. Unresolved history or pool
// exhaustion permanently disables metadata. No lock spans native/COM work.
struct BindingRange {binding::Footprint slots;bool write=false;};
// The normal write/simple-copy path needs at most two ranges and allocates no
// footprint storage. Larger range arrays reserve before taking the mutex.
struct BindingRanges {
    std::array<BindingRange,2> local{};
    std::vector<BindingRange> overflow;
    size_t count=0;
    void reserve(size_t n) {if(n>local.size())overflow.reserve(n);}
    void push_back(BindingRange v) {
        if(overflow.capacity())overflow.push_back(v);else local[count]=v;
        ++count;
    }
    const BindingRange* begin() const noexcept {return overflow.capacity()?overflow.data():local.data();}
    const BindingRange* end() const noexcept {return begin()+count;}
    void clear() noexcept {overflow.clear();count=0;}
};
struct BindingRecord {
    bool used=false,active=false,known=false,tainted=false,validated=false,nestingSafe=false;
    DWORD thread=0;
    uint64_t begin=0,end=0;
    uint64_t object=0,dependency=0;
    BindingRanges ranges;
    diagnostics::DisableEvent kind=diagnostics::DisableEvent::unknown;
    binding::ObservationKind eventKind=binding::ObservationKind::other;
};
void MetadataEvent::tag(const BindingRecord& other) const noexcept {
    overlap=binding::overlap_reason(binding::ObservationKind::root_create,other.eventKind,
                                   footprint.thread==other.thread);
}
struct BindingCohort {
    std::array<BindingRecord,256> records;
    // Fixed storage, no hot-path allocation. Retain until all overlapping
    // metadata observations retire, even when their outputs are already known.
    std::array<MetadataFootprint,64> completed{};
    size_t cursor=0;
    uint64_t lostThrough=0;
    unsigned active=0;
    uint64_t clock=0;
};
// Construct the fixed pools before interception; no reserve/initialization on
// an observation's locked hot path.
BindingCohort g_bindingCohort;
BindingCohort& bindingCohort() {return g_bindingCohort;}
void compareRootBinding(const BindingRecord& r) noexcept { // mutex held
    for(auto* e=state().metadataEvents;e;e=e->next) {
        const bool reentry=r.active && r.thread==e->footprint.thread;
        bool keyConflict=false;
        for(auto key:e->footprint.objects)if(key && r.known) {
            keyConflict|=key==r.object || key==r.dependency;
            for(const auto& range:r.ranges)keyConflict|=key==range.slots.heap.id;
        }
        if(reentry || keyConflict) {
            e->mark((reentry?MetadataEvent::binding_reentry:0u)|
                    (keyConflict?(r.thread==e->footprint.thread?
                        MetadataEvent::binding_key_same:MetadataEvent::binding_key_other):0u));
            e->blocking="binding_conflict";e->tag(r);
        }
    }
}
// Candidate COM is always outside the proxy mutex and never retried.
void srvCandidateCounts_locked() noexcept {
    auto* log=diagnostics::sink();if(!log||!log->ownership_pins())return;
    char text[256]{};state().srvCandidates.counts(text,sizeof text);
    diagnostics::emit(diagnostics::record(diagnostics::Event::requirements_metadata,0,0,0,text));
}
void releaseSrvCandidate(SrvCandidates::Reservation r) noexcept {
    if(!r.evicted)return;
    bool ok=true;try{r.evicted->Release();}catch(...){ok=false;}
    std::lock_guard lock(state().mutex);
    state().srvCandidates.released(r,ok);srvCandidateCounts_locked();
}
void retireSrvCandidates(uint64_t serial,uint64_t heapKey) noexcept {
    const auto* log=diagnostics::sink();
    if(!serial||!log||!log->ownership_pins())return;
    {auto& s=state();std::lock_guard lock(s.mutex);
     s.nativeBindings.diagnostic_retire_heap(heapKey,serial);
     s.srvCandidates.retire(serial);}
    for(unsigned slot=0;slot<SrvCandidates::slots_per_heap;++slot) {
        SrvCandidates::Reservation r;
        {std::lock_guard lock(state().mutex);r=state().srvCandidates.discard(serial,slot);}
        releaseSrvCandidate(r);
    }
}
struct BindingEvent {
    BindingRecord* record=nullptr;
    static constexpr unsigned maxNesting=8; // including the outermost event
    explicit BindingEvent(binding::ObservationKind k) noexcept:BindingEvent(0,false,0,nullptr,0,nullptr,nullptr,0,nullptr,nullptr,k) {}
    BindingEvent(uint64_t object,bool known,binding::ObservationKind k) noexcept:BindingEvent(object,known,0,nullptr,0,nullptr,nullptr,0,nullptr,nullptr,k) {}

    // Reserve outside the lock; allocation failure is metadata failure only.
    BindingEvent(uint64_t object=0,bool known=false,uint64_t dependency=0,
                 ID3D12Device* device=nullptr,UINT dc=0,const D3D12_CPU_DESCRIPTOR_HANDLE* dest=nullptr,const UINT* ds=nullptr,
                 UINT sc=0,const D3D12_CPU_DESCRIPTOR_HANDLE* src=nullptr,const UINT* ss=nullptr,
                 binding::ObservationKind eventKind=binding::ObservationKind::count,int copyType=-1) noexcept {
        auto& s=state();
        BindingRanges ranges;
        bool failed=false;
        const auto kind=device?((sc || eventKind==binding::ObservationKind::copy)?diagnostics::DisableEvent::descriptor_copy:diagnostics::DisableEvent::descriptor_write):
            object?diagnostics::DisableEvent::release:diagnostics::DisableEvent::creation;
        const bool shape=dc<=65536 && sc<=65536 && (!dc||dest) && (!sc||src);
        try {
            if(shape) ranges.reserve(size_t(dc)+sc);
        } catch(...) {failed=true;}
        std::lock_guard lock(s.mutex);
        s.bindingEvents.fetch_add(1);
        auto& cohort=bindingCohort();++cohort.active;
        for(auto& r:cohort.records) if(!r.used) {record=&r;break;}
        if(!record && s.metadataEvents) {
            // Root history may lose evidence under pressure; it must never
            // consume the pool reserved for binding-cohort correctness.
            reclaim(true);
            for(auto& r:cohort.records) if(!r.used) {record=&r;break;}
        }
        if(!record || failed) {
            // The missing footprint must also block every overlapping root.
            cohort.lostThrough=cohort.clock==UINT64_MAX?UINT64_MAX:++cohort.clock;
            record=nullptr;disable(failed?"binding footprint allocation failed":"binding footprint capacity exhausted",false,kind);return;
        }
        auto& r=*record;r.eventKind=eventKind==binding::ObservationKind::count?
            (device?(sc?binding::ObservationKind::copy:binding::ObservationKind::descriptor_write):
             object?binding::ObservationKind::resource_release:binding::ObservationKind::committed_create):eventKind;
        r.kind=kind;r.used=r.active=true;r.validated=false;r.known=known&&shape;r.thread=GetCurrentThreadId();
        if(cohort.clock==UINT64_MAX) disable("binding ordering identity exhausted",false,kind);
        else r.begin=++cohort.clock;
        r.end=0;
        r.object=object;r.dependency=dependency;r.ranges=std::move(ranges);
        const auto owner=reinterpret_cast<uintptr_t>(device);
        auto append=[&](UINT count,const D3D12_CPU_DESCRIPTOR_HANDLE* handles,const UINT* sizes,bool write) {
            for(UINT n=0;n<count;++n) {
                auto f=s.nativeBindings.footprint(handles[n].ptr,sizes?sizes[n]:1,owner);
                if(!f) r.known=false;
                r.ranges.push_back({f,write}); // reserved; no allocation under lock
            }
        };
        if(shape) {append(dc,dest,ds,true);append(sc,src,ss,false);}
        r.nestingSafe=r.known && kind!=diagnostics::DisableEvent::creation;
        if(kind==diagnostics::DisableEvent::descriptor_copy) {
            // Malformed copies can invalidate ALL registry slots. Prove the
            // bounded one-range shape before allowing nesting; larger shapes
            // keep the existing fail-closed policy. No native/COM calls here.
            const auto destinations=r.known && dc==1?(ds?ds[0]:1):0;
            const auto sources=r.known && sc==1?(ss?ss[0]:1):0;
            r.nestingSafe=r.nestingSafe && dc==1 && sc==1 && copyType>=0 && copyType<=1 &&
                destinations && destinations<=65536 && destinations==sources;
            if(r.nestingSafe) {
                const auto* pair=r.ranges.begin();
                r.nestingSafe=pair[0].slots.end<=pair[1].slots.begin || pair[1].slots.end<=pair[0].slots.begin;
            }
        }
        unsigned depth=1;
        for(const auto& other:cohort.records) if(&other!=record && other.active && other.thread==r.thread)++depth;
        for(auto& other:cohort.records) if(&other!=record && other.active && other.thread==r.thread) {
            // Creation's returned object alone does not describe its eventual
            // heap/address interval. Do not infer independence after identify().
            // Descriptor/descriptor and release/release disjointness are fully
            // described here. Mixed release/descriptor needs address-dependency
            // tracking (CBV), so remains fail-closed rather than guessing.
            const bool descriptors=!r.object&&!other.object && r.kind!=diagnostics::DisableEvent::creation && other.kind!=diagnostics::DisableEvent::creation;
            const bool releases=r.kind==diagnostics::DisableEvent::release && other.kind==diagnostics::DisableEvent::release;
            if(depth>maxNesting || !r.nestingSafe || !other.nestingSafe || !(descriptors||releases) || conflicts(r,other)) {
                disable(binding::overlap_reason(r.eventKind,other.eventKind,true),true,kind);break;
            }
        }
        compare();
        compareRootBinding(r);
    }
    BindingEvent(const BindingEvent&)=delete;
    BindingEvent& operator=(const BindingEvent&)=delete;
    static void disable(const char* why,bool reportAlways=false,diagnostics::DisableEvent kind=diagnostics::DisableEvent::unknown) noexcept {
        auto& b=state().nativeBindings;
        if(!b.enabled() && !reportAlways)return;
        b.disable(why);
        diagnostics::observer_disabled(why,kind);
    }
    static void invalidate(BindingRecord& r) noexcept {
        auto& b=state().nativeBindings;
        if(!r.known) {disable("unknown binding footprint",false,r.kind);return;}
        if(r.object) b.invalidate_object(r.object);
        for(const auto& range:r.ranges) if(range.write) b.invalidate(range.slots);
    }
    static void reclaim(bool pressure=false) noexcept { // caller holds mutex
        auto& s=state();auto& c=bindingCohort();
        // Validate a finished binding cohort even when metadata still needs its
        // history. In particular U(unknown) may finish after A(known), so the
        // per-event active>1 check alone cannot detect their unresolved overlap.
        if(!c.active) {
            unsigned used=0;bool unknown=false;
            for(const auto& r:c.records) if(r.used&&!r.validated) {++used;unknown|=!r.known;}
            if(unknown && used>1) for(const auto& r:c.records) if(r.used&&!r.validated&&!r.known)unknownOverlap(r);
            // Metadata can retain several already-finished binding cohorts.
            // Never combine non-overlapping calls into a new unknown overlap.
            for(auto& r:c.records) if(r.used)r.validated=true;
        }
        bool pending=false;
        for(const auto& r:c.records) if(r.active&&!r.known)pending=true;
        // Unknown binding outputs still need completed records for J-047's
        // late-address comparison. Pressure cannot discard that evidence.
        if(pending || (s.metadataEvents&&!pressure))return;
        for(auto& r:c.records) if(r.used&&!r.active) {
            if(r.tainted)invalidate(r);
            if(s.metadataEvents) c.lostThrough=(std::max)(c.lostThrough,r.end);
            r.ranges.clear();r.used=r.known=r.tainted=r.validated=false;r.object=r.dependency=0;
        }
    }
    static bool conflicts(const BindingRecord& a,const BindingRecord& b) noexcept {
        bool conflict=a.object && (a.object==b.object || a.object==b.dependency);
        conflict=conflict || (b.object && b.object==a.dependency);
        for(const auto& x:a.ranges) {
            if(b.object==x.slots.heap.id && b.object) conflict=true;
            for(const auto& y:b.ranges) if((x.write||y.write) && x.slots.begin<y.slots.end && y.slots.begin<x.slots.end)
                conflict=true;
        }
        for(const auto& y:b.ranges) if(a.object==y.slots.heap.id && a.object) conflict=true;
        return conflict;
    }
    static void unknownOverlap(const BindingRecord& a) noexcept { // mutex held
        for(const auto& b:bindingCohort().records)
            if(&a!=&b && b.used && (!b.end || b.end>a.begin) && (!a.end || a.end>b.begin)) {
                disable(binding::overlap_reason(a.eventKind,b.eventKind,a.thread==b.thread),false,a.kind);return;
            }
    }
    void compare() noexcept { // caller holds state mutex
        if(!record || !record->known) return;
        auto& a=*record;
        for(auto& b:bindingCohort().records) {
            if(&b==record || !b.used || !b.known || (b.end && b.end<=a.begin) || (a.end && a.end<=b.begin)) continue;
            const bool conflict=conflicts(a,b);
            if(conflict) {a.tainted=b.tainted=true;invalidate(a);invalidate(b);}
        }
    }
    // Invalidate before native forwarding so a concurrent Draw cannot promote
    // an overwritten publication. Pending AddRef is cancelled, never reused.
    void prepareCandidates() noexcept {
        auto* log=diagnostics::sink();if(!log||!log->ownership_pins()||!record)return;
        for(const auto& range:record->ranges)if(range.write&&range.slots) {
            const auto& f=range.slots;
            uint64_t serial=0;
            {std::lock_guard lock(state().mutex);
             if(const auto* d=state().nativeBindings.diagnostic()) {
                 const auto h=d->heaps.find(f.heap.id);
                 if(h!=d->heaps.end()&&h->second.live)serial=h->second.serial;
             }}
            if(!serial)continue;
            const bool supported=f.type==D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV&&
                (f.flags&D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
            // Copy cost is proportional to destination slots, bounded by cap.
            // Out-of-cap slots cannot own references; count their maximum too.
            const auto end=uint64_t(f.first)+f.count;
            const auto bounded=static_cast<unsigned>((std::min)(end,uint64_t(SrvCandidates::slots_per_heap)));
            for(unsigned slot=f.first;slot<bounded;++slot) {
                SrvCandidates::Reservation r;
                {std::lock_guard lock(state().mutex);
                 state().srvCandidates.written(serial,slot,supported);
                 r=state().srvCandidates.discard(serial,slot);}
                releaseSrvCandidate(r);
            }
            {std::lock_guard lock(state().mutex);
             if(end>SrvCandidates::slots_per_heap&&f.count)
                 state().srvCandidates.written(serial,static_cast<unsigned>(end-1),supported);
             srvCandidateCounts_locked();}
        }
    }
    void identify(uint64_t object) noexcept {
        auto& s=state();std::lock_guard lock(s.mutex);
        if(record) {record->object=object;record->known=true;compare();compareRootBinding(*record);}
    }
    ~BindingEvent() {
        auto& s=state();std::lock_guard lock(s.mutex);
        auto& c=bindingCohort();
        if(record) {
            record->active=false;
            record->end=c.clock==UINT64_MAX?UINT64_MAX:++c.clock;
            // Unknown descriptor intervals cannot be safely ordered against ANY
            // other observation. A lone malformed call retains registry policy.
            if(!record->known && c.active>1) unknownOverlap(*record);
            if(record->tainted) invalidate(*record);
        }
        --c.active;
        reclaim();
        s.bindingEvents.fetch_sub(1);
    }
};
uint64_t cohortTick() noexcept {
    auto& c=bindingCohort();return c.clock==UINT64_MAX?UINT64_MAX:++c.clock;
}
bool touches(const MetadataFootprint& a,const MetadataFootprint& b) noexcept {
    for(auto x:a.objects) for(auto y:b.objects) if(x && x==y)return true;
    return false;
}
std::array<unsigned,4> MetadataEvent::overlapCounts() const noexcept {
    std::array<unsigned,4> counts{};
    auto count=[&](unsigned slot) {if(counts[slot]<255)++counts[slot];};
    for(auto* other=state().metadataEvents;other;other=other->next)if(other!=this)
        count(other->footprint.thread==footprint.thread?0:1);
    for(const auto& other:bindingCohort().completed)if(other.end>footprint.begin)
        count(other.thread==footprint.thread?2:3);
    return counts;
}
void MetadataEvent::mark(unsigned reason,const MetadataFootprint* other) noexcept {
    ambiguous=true;ambiguityReasons|=reason;
    if(other) {
        const auto kind=std::string_view(other->kind);
        ambiguityKinds|=kind=="root_create"?1u:kind=="pso_create"?2u:kind=="release"?4u:8u;
    }
    const auto counts=overlapCounts();
    for(size_t i=0;i<counts.size();++i)ambiguityHigh[i]=(std::max)(ambiguityHigh[i],counts[i]);
}
void MetadataEvent::appendAmbiguity(char* text,size_t size) const noexcept {
    const auto used=std::strlen(text);if(used>=size)return;
    const auto counts=overlapCounts();
    std::snprintf(text+used,size-used,";ar=%03x;ak=%01x;mc=%02x%02x%02x%02x;mh=%02x%02x%02x%02x;nr=%02x",
        ambiguityReasons,ambiguityKinds,counts[0],counts[1],counts[2],counts[3],
        ambiguityHigh[0],ambiguityHigh[1],ambiguityHigh[2],ambiguityHigh[3],nestedReleases);
    auto pos=std::strlen(text);if(pos>=size)return;
    std::snprintf(text+pos,size-pos,";nf=%04x;nc=",nestedRefusals);
    for(auto count:nestedReasonCounts) {
        pos=std::strlen(text);if(pos>=size)return;
        std::snprintf(text+pos,size-pos,"%02x",count);
    }
    pos=std::strlen(text);if(pos<size)std::snprintf(text+pos,size-pos,";np=%02x",passedChildren);
    pos=std::strlen(text);if(pos<size)
        std::snprintf(text+pos,size-pos,";uc=%02x%02x%02x",
            unregisteredSightings[0],unregisteredSightings[1],unregisteredSightings[2]);
    // Called only at creation resolution. Draw copies the retained detail.
    const auto& table=state().creationSightings;
    pos=std::strlen(text);if(pos<size)
        std::snprintf(text+pos,size-pos,";uu=%02x%02x%02x%02x;st=%04x%04x%04x",
            unavailableSightings[0],unavailableSightings[1],unavailableSightings[2],unavailableSightings[3],
            table.occupancy,table.highWater,table.evictions);
}
bool registeredOwnershipKey_locked(uint64_t key) noexcept {
    if(!key)return false;
    auto& s=state();auto* p=reinterpret_cast<IUnknown*>(key);
    if(s.listKeys.count(p)||s.states.count(reinterpret_cast<ID3D12GraphicsCommandList*>(p))||
       s.rootKeys.count(p)||s.rootIdentities.count(reinterpret_cast<ID3D12RootSignature*>(p))||
       s.roots.count(reinterpret_cast<ID3D12RootSignature*>(p))||
       s.gs.count(reinterpret_cast<ID3D12PipelineState*>(p))||s.compatibilityOwners.count(p)||
       s.nativeBindings.heap_token(key).id||s.nativeBindings.resource_token(key).id)return true;
    for(const auto& entry:s.listKeys)if(reinterpret_cast<uintptr_t>(entry.second)==key)return true;
    for(const auto& entry:s.rootKeys)if(reinterpret_cast<uintptr_t>(entry.second)==key)return true;
    auto pipelineClaims=[&] {
        perf::Scope cost(perf::pso_scan);
        for(const auto& entry:s.pipelines) {
            if(reinterpret_cast<uintptr_t>(entry.first)==key)return true;
            for(auto k:entry.second.retirementKeys)if(k==key)return true;
        }
        return false;
    };
    // FPS growth fix + section 8: the index's claim count IS the walk's answer (a claimed key used to run the walk only to confirm it);
    // the whole-map walk runs only when the index is unavailable.
    if(const auto* x=pipelineIndex_locked()) {
        bool claimed=x->retirement.count(uintptr_t(key))!=0;
#ifdef ACZ_VR_OFFLINE_TEST
        if(const bool scan=pipelineClaims();scan!=claimed){++g_pipelineIndexMismatch;claimed=scan;} // the walk's answer wins in tests
#endif
        if(claimed)return true;
    } else if(pipelineClaims())return true;
    for(const auto& entry:s.rootCreations) {
        if(reinterpret_cast<uintptr_t>(entry.first)==key||
           reinterpret_cast<uintptr_t>(entry.second.interfaceAddress)==key)return true;
        for(auto k:entry.second.retirementKeys)if(k==key)return true;
    }
    for(const auto& pin:s.ownershipPins)if(pin.reserved) {
        if(reinterpret_cast<uintptr_t>(pin.texture)==key||reinterpret_cast<uintptr_t>(pin.buffer)==key)return true;
        if(reinterpret_cast<uintptr_t>(pin.root.get())==key||reinterpret_cast<uintptr_t>(pin.rootKey)==key||
           reinterpret_cast<uintptr_t>(pin.rootCanonical)==key)return true;
        for(auto* address:pin.interfaces)if(reinterpret_cast<uintptr_t>(address)==key)return true;
    }
    return false;
}
void MetadataEvent::resolveNestedReleases() noexcept {
    if(!nestedReleases||nestedResolved)return;
    nestedResolved=true;
    // Never clear a mark. Only the constructor's one deferred writer can be
    // avoided, once BOTH immutable footprints and all retained history exist.
    auto& c=bindingCohort();
    unsigned completed=0;
    bool safe=footprint.known&&!footprint.overflow&&serial&&address&&
        c.clock!=UINT64_MAX&&c.lostThrough<footprint.begin;
    unsigned common=0;
    if(!footprint.known||footprint.overflow||!address)common|=parent_incomplete;
    if(!serial||c.clock==UINT64_MAX)common|=clock_serial;
    if(c.lostThrough>=footprint.begin)common|=missing_history;
    if(nestedReleases==255)common|=certificate_overflow;
    // Count each reason at most once per audited child, independently of how
    // many peers/keys exhibit it. Missing children get conservative aggregate
    // reasons; no guessed identity is attached to an overwritten footprint.
    auto record=[&](unsigned reasons) {
        nestedRefusals|=reasons;
        for(size_t i=0;i<nestedReasonCounts.size();++i)
            if((reasons&(1u<<i))&&nestedReasonCounts[i]<255)++nestedReasonCounts[i];
        if(!reasons&&passedChildren<255)++passedChildren;
    };
    auto nested=[&](const MetadataFootprint& f) {
        return f.thread==footprint.thread&&f.begin>footprint.begin&&
            std::string_view(f.kind)=="release";
    };
    for(auto* e=state().metadataEvents;e;e=e->next)if(e!=this&&nested(e->footprint))safe=false;
    for(const auto& f:c.completed)if(f.end>footprint.begin&&nested(f)) {
        ++completed;
        if(f.releaseRefusals&unregistered) {
            auto& count=unregisteredSightings[(std::min)(f.creationSighting,2u)];
            if(count<255)++count;
            if(f.creationSighting>=2) {
                unsigned causes=f.sightingUnavailable;
                if(f.sightingClock==UINT64_MAX||f.sightingClock>f.begin)
                    causes|=CreationSightings::clock_mismatch;
                if(f.creationSighting>2||!causes||(causes&~15u))causes|=CreationSightings::other;
                for(size_t i=0;i<unavailableSightings.size();++i)
                    if((causes&(1u<<i))&&unavailableSightings[i]<255)++unavailableSightings[i];
            }
        }
        bool owned=ownershipPinned&&f.ownershipCandidate&&f.known&&!f.overflow&&
            (f.releaseRefusals&unregistered)&&
            !(f.releaseRefusals&~unsigned(unregistered|child_incomplete));
        if(owned)for(auto key:f.objects)if(key&&registeredOwnershipKey_locked(key))owned=false;
        // Only the two missing observation-certificate clauses gain an alternate
        // proof. Every other refusal, conflict, history and ambiguity stays.
        unsigned reasons=common|f.releaseRefusals;
        if(owned)reasons&=~unsigned(unregistered|child_incomplete);
        if((!f.releaseComplete&&!owned)||!f.known)reasons|=child_incomplete;
        if(f.overflow)reasons|=certificate_overflow;
        if(touches(footprint,f))reasons|=parent_key;
        if((!f.releaseComplete&&!owned)||!f.known||f.overflow||touches(footprint,f))safe=false;
        // The exception needs the child's keys to be independent of retained
        // metadata/binding history too, even if that peer ended before this
        // child began and could not have marked it through touch().
        for(auto* e=state().metadataEvents;e;e=e->next)if(e!=this) {
            if(!e->footprint.known)reasons|=unknown_peer;
            if(touches(f,e->footprint))reasons|=retained_key;
        }
        for(const auto& peer:c.completed)if(peer.end>footprint.begin&&peer.begin!=f.begin) {
            if(!peer.known)reasons|=unknown_peer;
            if(touches(f,peer))reasons|=retained_key;
        }
        for(const auto& r:c.records)if(r.used&&(!r.end||r.end>footprint.begin)) {
            if(!r.known)reasons|=unknown_peer;
            for(auto key:f.objects)if(key) {
                if(key==r.object||key==r.dependency)reasons|=retained_key;
                for(const auto& range:r.ranges)if(key==range.slots.heap.id)reasons|=retained_key;
            }
        }
        if(reasons)safe=false;
        else if(owned&&ownershipChildren<255)++ownershipChildren;
        record(reasons);
    }
    for(unsigned i=completed;i<nestedReleases;++i)record(common|child_incomplete);
    // A lost/replaced child, unfinished child or saturated counter is no proof.
    if(completed!=nestedReleases||nestedReleases==255)safe=false;
    if(!safe) {
        MetadataFootprint release;release.kind="release";
        mark(reentry,&release);blocking="reentry";
    } else provenNestedReleases=nestedReleases;
}
MetadataEvent::MetadataEvent(IUnknown* value,bool create,const char* kind):creation(create) {
    auto& s=state();std::lock_guard lock(s.mutex);
    isolated=!s.rootCreates && !s.metadataReleases && !s.bindingEvents.load();
    serial=s.rootEventSerial=capture::new_lifetime_identity();
    footprint.begin=cohortTick();footprint.thread=GetCurrentThreadId();
    footprint.kind=kind?kind:create?"root_create":"release";
    if(creation) ++s.rootCreates;else ++s.metadataReleases;
    next=s.metadataEvents;s.metadataEvents=this;
    for(auto* other=next;other;other=other->next) if(other->footprint.thread==footprint.thread) {
        mark(reentry,&other->footprint);
        if(std::string_view(footprint.kind)=="release"&&
           std::string_view(other->footprint.kind)=="pso_create") {
            if(other->nestedReleases<255)++other->nestedReleases;
            else other->mark(capacity);
            // Parent output/aliases may still be unknown. Retain this child;
            // the parent must resolve the decision before publishing anything.
        } else other->mark(reentry,&footprint);
        blocking=other->blocking="reentry";
        tag(other->footprint);other->tag(footprint);
    }
    identify(value);
}
void MetadataEvent::touch(IUnknown* value) {
    if(!value)return;
    const auto key=reinterpret_cast<uintptr_t>(value);
    bool stored=false;
    for(auto& slot:footprint.objects) {
        if(slot==key) {stored=true;break;}
        if(!slot) {slot=key;stored=true;break;}
    }
    if(!stored) {footprint.overflow=true;footprint.known=false;mark(capacity);blocking="footprint_capacity";}
    for(auto* other=state().metadataEvents;other;other=other->next)
        if(other!=this && touches(footprint,other->footprint)) {
            const auto reason=other->footprint.thread==footprint.thread?active_same:active_other;
            mark(reason,&other->footprint);other->mark(reason,&footprint);
            blocking=other->blocking="object_conflict";
            tag(other->footprint);other->tag(footprint);
        }
    for(const auto& other:bindingCohort().completed)
        if(other.end>footprint.begin && touches(footprint,other)) {
            mark(other.thread==footprint.thread?completed_same:completed_other,&other);
            blocking="completed_conflict";
            tag(other);
        }
}
void MetadataEvent::identify(IUnknown* value) {
    address=value;footprint.known=value!=nullptr&&!footprint.overflow;touch(value);
}
bool MetadataEvent::distinct(const HookState& s) const {
    auto& c=bindingCohort();
    if(!serial || !address || !footprint.known) {blocking="unknown_self";return false;}
    if(c.clock==UINT64_MAX || c.lostThrough>=footprint.begin) {blocking="history_lost";return false;}
    if(nestedReleases!=provenNestedReleases) {blocking="reentry";return false;}
    if(ambiguous)return false;
    unsigned activeBindings=0;
    for(const auto& r:c.records)if(r.active)++activeBindings;
    if(activeBindings!=c.active) {blocking="binding_record_missing";return false;}
    for(auto* other=s.metadataEvents;other;other=other->next) if(other!=this) {
        tag(other->footprint);
        if(!other->footprint.known) {
            blocking=other->creation?"active_root_unknown":
                std::string_view(other->footprint.kind)=="pso_create"?"active_pso_unknown":"active_metadata_unknown";
            return false;
        }
        if(touches(footprint,other->footprint)) {blocking="active_conflict";return false;}
    }
    for(const auto& other:c.completed) if(other.end>footprint.begin) {
        tag(other);
        if(!other.known) {blocking="completed_unknown";return false;}
        if(touches(footprint,other)) {blocking="completed_conflict";return false;}
    }
    for(const auto& other:c.records) if(other.used && (!other.end || other.end>footprint.begin)) {
        tag(other);
        if(other.thread==footprint.thread && other.active) {blocking="binding_reentry";return false;}
        if(!other.known) {
            blocking=other.kind==diagnostics::DisableEvent::creation?"binding_create_unknown":
                other.kind==diagnostics::DisableEvent::descriptor_copy?"descriptor_copy_unknown":"binding_unknown";
            return false;
        }
        for(auto key:footprint.objects) if(key) {
            if(key==other.object || key==other.dependency) {blocking="binding_conflict";return false;}
            for(const auto& range:other.ranges) if(key==range.slots.heap.id) {blocking="heap_conflict";return false;}
        }
        // Binding creation's identify() establishes its returned address, not
        // canonical COM identity. Its observer deliberately rejects aliases;
        // do not infer a disjoint canonical key from that native output alone.
        // Root/PSO MetadataEvents validate their canonical keys separately.
        if(other.kind==diagnostics::DisableEvent::creation && other.object) {
            blocking="binding_create_identity_unknown";return false;
        }
    }
    blocking="none";return true;
}
// Independent diagnostic components of distinct(); never used for eligibility.
diagnostics::LifetimeConditions MetadataEvent::orderConditions(const HookState& s) const {
    diagnostics::LifetimeConditions out;
    auto& c=bindingCohort();
    out.set(1u<<0,serial!=0);out.set(1u<<1,address!=nullptr);
    out.set(1u<<2,footprint.known);out.set(1u<<3,c.clock!=UINT64_MAX);
    out.set(1u<<4,c.lostThrough<footprint.begin);
    out.set(1u<<5,!ambiguous&&nestedReleases==provenNestedReleases);
    unsigned active=0;for(const auto& r:c.records)if(r.active)++active;
    out.set(1u<<6,active==c.active);
    bool activeKnown=true,activeDisjoint=true,completedKnown=true,completedDisjoint=true;
    bool noReentry=true,bindingKnown=true,bindingDisjoint=true,creationKnown=true;
    for(auto* other=s.metadataEvents;other;other=other->next)if(other!=this) {
        activeKnown&=other->footprint.known;
        activeDisjoint&=!touches(footprint,other->footprint);
    }
    for(const auto& other:c.completed)if(other.end>footprint.begin) {
        completedKnown&=other.known;completedDisjoint&=!touches(footprint,other);
    }
    for(const auto& other:c.records)if(other.used&&(!other.end||other.end>footprint.begin)) {
        noReentry&=!(other.thread==footprint.thread&&other.active);
        bindingKnown&=other.known;
        for(auto key:footprint.objects)if(key) {
            bindingDisjoint&=key!=other.object&&key!=other.dependency;
            for(const auto& range:other.ranges)bindingDisjoint&=key!=range.slots.heap.id;
        }
        creationKnown&=!(other.kind==diagnostics::DisableEvent::creation&&other.object);
    }
    const bool values[]={activeKnown,activeDisjoint,completedKnown,completedDisjoint,
        noReentry,bindingKnown,bindingDisjoint,creationKnown};
    for(unsigned i=0;i<std::size(values);++i)out.set(1u<<(7+i),values[i]);
    return out;
}
MetadataEvent::~MetadataEvent() {
    auto& s=state();std::lock_guard lock(s.mutex);auto& c=bindingCohort();
    if(std::string_view(footprint.kind)=="release")
        footprint.releaseComplete=footprint.releaseComplete&&!(ambiguityReasons&~reentry);
    if(std::string_view(footprint.kind)=="release"&&(ambiguityReasons&~reentry))
        footprint.releaseRefusals|=child_ambiguity;
    if(std::string_view(footprint.kind)=="release"&&(!serial||c.clock==UINT64_MAX))
        footprint.releaseRefusals|=clock_serial;
    footprint.end=cohortTick();
    auto** link=&s.metadataEvents;
    while(*link!=this) link=&(*link)->next;
    *link=next;
    // Keep completed observations whose end follows any active begin. A full
    // ring loses ordering evidence, never grants independence.
    if(s.metadataEvents) {
        uint64_t oldest=UINT64_MAX;
        for(auto* e=s.metadataEvents;e;e=e->next)oldest=(std::min)(oldest,e->footprint.begin);
        auto& slot=c.completed[c.cursor];
        if(slot.end>oldest)c.lostThrough=(std::max)(c.lostThrough,slot.end);
        slot=footprint;c.cursor=(c.cursor+1)%c.completed.size();
    } else {
        c.completed.fill(MetadataFootprint{});c.cursor=0;c.lostThrough=0;

    }
    // Identical validation/reclamation policy at either retirement seam.
    BindingEvent::reclaim();
    // Token-checked rollback: an older event must not erase a newer record.
    if(creation && ambiguous && address) {
        auto* root=reinterpret_cast<ID3D12RootSignature*>(address);
        auto found=s.rootCreations.find(root);
        if(found!=s.rootCreations.end() && found->second.publication==serial) {
            const auto identity=found->second.identity;
            s.rootCreations.erase(found);s.rootIdentities.erase(root);s.roots.erase(root);
            for(auto& list:s.states) if(list.second.bindings.root_id==reinterpret_cast<uintptr_t>(address) &&
                list.second.bindings.root_identity==identity) {
                list.second.bindings.root_identity=0;++list.second.bindings.revision;
            }
        }
    }
    if(creation) --s.rootCreates;else --s.metadataReleases;
}
template<diagnostics::DisableEvent Kind=diagnostics::DisableEvent::unknown,class F> void observeNativeBinding(F&& fn) noexcept {
    perf::Scope cost(perf::descriptor); // FPS investigation (observe only; includes the hook-mutex wait)
    auto& s=state();std::lock_guard lock(s.mutex);
    auto* log=diagnostics::sink();s.nativeBindings.diagnostic_tracking(log&&log->ownership_pins());
    const bool wasEnabled=s.nativeBindings.enabled();
    try {fn(s.nativeBindings);}
    catch(...) {s.nativeBindings.disable("native observation exception");}
    if(wasEnabled&&!s.nativeBindings.enabled())diagnostics::observer_disabled(s.nativeBindings.disable_reason(),Kind);
}
HMODULE g_realDXGI = nullptr;
std::once_flag g_loadOnce;
thread_local bool g_loading=false;
#ifdef ACZ_VR_OFFLINE_TEST
// Scoped fixture seams: never call the system loader in an offline fixture.
HMODULE (*g_loadTest)() = nullptr;
std::once_flag* g_loadOnceTest = &g_loadOnce;
void (*g_initializeTest)() = nullptr;
bool (*g_allocateFactoryTest)() = nullptr;
void (*g_wrapperQueryTest)(bool published) = nullptr;
#endif
thread_local unsigned g_callbackDepth = 0;
struct CallbackGuard {
    bool outer = g_callbackDepth++ == 0;
    ~CallbackGuard() { --g_callbackDepth; }
};
template<class F> void observeSafely(F&& work) noexcept {
    try { work(); }
    catch (...) { OutputDebugStringA("[ACZ-VR] observation failed; forwarding continues\n"); }
}
template<class F, class... A> void notify(const F& cb, A&&... args) noexcept {
    if (!cb) return;
    CallbackGuard guard;
    if (!guard.outer) return;
    try { cb(std::forward<A>(args)...); }
    catch (...) { OutputDebugStringA("[ACZ-VR] callback failed; forwarding continues\n"); }
}
struct ReleaseCOM {
    template<class T> void operator()(T* p) const noexcept { if (p) p->Release(); }
};
void** vtable(void* obj) { return *static_cast<void***>(obj); }
// Absolute SDK ABI slots, including inherited methods.
constexpr int VTIDX_Release=2, VTIDX_Reset=10, VTIDX_ClearState=11;
constexpr int VTIDX_DrawInstanced=12, VTIDX_DrawIndexedInstanced=13;
constexpr int VTIDX_SetPipelineState=25, VTIDX_ExecuteCommandLists=10;
using PFN_Release=ULONG(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
using PFN_Reset=HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using PFN_ClearState=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
using PFN_SetPipelineState=PFN_ClearState;
using PFN_DrawInstanced=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
using PFN_DrawIndexedInstanced=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
using PFN_ExecuteCommandLists=void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
#ifdef ACZ_VR_OFFLINE_TEST
auto g_protect = &VirtualProtect;
bool g_pinResult = true;
// Foundation fixtures historically omit certification; the full certification
// fixture opts into the production Execute discovery path below.
bool g_captureDiscoveryTest = false;
#else
constexpr auto g_protect = &VirtualProtect;
bool g_pinResult = true;
#endif
bool pinProxy() {
#ifndef ACZ_VR_OFFLINE_TEST
    HMODULE module=nullptr;
    // Shared tables may outlive the factory: forbid unloading installed hooks.
    const bool result=GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&pinProxy), &module)!=0;
#else
    const bool result=g_pinResult;
#endif
    // Re-checking a pinned module is idempotent. Report transitions only,
    // including a failure/recovery in the offline seam, without another lock.
    static std::atomic<int> last{-1};
    if(last.exchange(result?1:0,std::memory_order_relaxed)!=(result?1:0))
        diagnostics::emit(diagnostics::record(diagnostics::Event::hook_pin,result));
    return result;
}
// Under mutex. Publish originals before modifying shared tables. Partial installs
// retain dispatch records; failed protection/restoration never reports success.
bool patch(void** table, int slot, void* replacement, Hooks& hooks) {
    // Fixed ring enqueue only, even when the caller holds the hook mutex.
    // The destructor reports false on every partial/protection/restore exit.
    struct Outcome {int slot;Hooks& hooks;bool ok=false;~Outcome(){
        const unsigned char next=ok?1:2;
        if(hooks.outcomes[slot]==next)return;
        hooks.outcomes[slot]=next;
        diagnostics::emit(diagnostics::record(diagnostics::Event::hook_patch,
            static_cast<uint64_t>(slot),ok,0,ok?"installed":"failed or partial patch"));
    }} outcome{slot,hooks};
    if (!hooks.originals[slot]) {
        if (!table[slot] || table[slot]==replacement) return false;
        hooks.originals[slot]=table[slot];
    }
    if (hooks.pendingProtection[slot]) {
        DWORD ignored=0;
        if (!g_protect(&table[slot], sizeof(void*), hooks.pendingProtection[slot], &ignored))
            return false;
        hooks.pendingProtection[slot]=0;
    }
    // Live slot + retained dispatch, not a cached installed flag. Restoration
    // above must succeed even when the replacement is already present. Callers
    // still check lifetime/prerequisite slots and discover each list's state.
    if (table[slot]==replacement) return outcome.ok=true;
    DWORD old=0;
    if (!g_protect(&table[slot], sizeof(void*), PAGE_READWRITE, &old)) return false;
    if (table[slot]!=hooks.originals[slot] && table[slot]!=replacement) {
        DWORD ignored=0;
        if (!g_protect(&table[slot], sizeof(void*), old, &ignored)) hooks.pendingProtection[slot]=old;
        return false;
    }
    hooks.pendingProtection[slot]=old;
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&table[slot]), replacement);
    DWORD ignored=0;
    if (!g_protect(&table[slot], sizeof(void*), old, &ignored)) return false;
    hooks.pendingProtection[slot]=0;
    return outcome.ok=true;
}
template<class F> F original(void* self, int slot, bool queue=false) {
    auto& s=state(); std::lock_guard lock(s.mutex);
    auto& map=queue?s.queues:s.lists;
    auto it=map.find(vtable(self));
    return it==map.end()?nullptr:reinterpret_cast<F>(it->second.originals[slot]);
}
template<class F> void observeBinding(ID3D12GraphicsCommandList*,F&&) noexcept;
void setPSO(ID3D12GraphicsCommandList* self, ID3D12PipelineState* pso) {
    // Invalidate before allocation/COM work, so an observation failure can never
    // leave a previous pipeline classified as the newly forwarded pipeline.
    {
        auto& s=state();std::lock_guard lock(s.mutex);
        auto& b=s.states[listKey_locked(self)].bindings;++b.revision;b.pipeline={};
    }
    std::shared_ptr<ID3D12PipelineState> owned;
    if (pso) {
        pso->AddRef();
        owned={pso, [](ID3D12PipelineState* p){if(ownershipShutdown.stopped())releaseOwned(p);else p->Release();}};
    }
    std::shared_ptr<ID3D12PipelineState> previous;
    {
        auto& s=state(); std::lock_guard lock(s.mutex);
        previous.swap(s.states[listKey_locked(self)].pso);
        s.states[listKey_locked(self)].pso=std::move(owned);
        auto& b=s.states[listKey_locked(self)].bindings;
        auto known=s.pipelines.find(pso);
        if(known!=s.pipelines.end()) {
            b.pipeline.drawcpt=known->second.shader.matches_drawcpt_geometry();
            b.pipeline.identity=known->second.identity;
            b.pipeline.explicit_root_identity=known->second.root_identity;
            b.pipeline.strip_cut=known->second.strip_cut;
            b.pipeline.topology_type=known->second.topology_type;
        }
    } // Release outside mutex.
}
// J-3b7: every recorded graphics variant table of the list carries the step-3
// RS/OM tracking hooks (slots 21, 22, 46). Data only; no COM.
bool step3TablesHooked_locked(const ListState& list) noexcept {
    auto& s=state();bool any=false;
    for(unsigned i=1;i<12;++i)if(auto* table=list.tables[i]) {
        auto it=s.lists.find(table);
        if(it==s.lists.end()||!it->second.originals[21]||!it->second.originals[22]||!it->second.originals[46])return false;
        any=true;
    }
    return any;
}
void resetBindings(ID3D12GraphicsCommandList* self, bool observedReset=false) {
    // Preserve the old empty ListState constructor's identity reservation,
    // including saturation behaviour, without constructing its large witnesses.
    (void)capture::new_lifetime_identity();
    // Only COM owners require deferred destruction. Keep them invocation-local
    // so a Release that reenters Reset cannot overwrite another call's owners.
    std::shared_ptr<ID3D12PipelineState> oldPso;
    std::shared_ptr<ID3D12RootSignature> oldRoot;
    std::vector<std::shared_ptr<ID3D12DescriptorHeap>> oldHeaps;
    auto& s=state();
    { std::lock_guard lock(s.mutex);
      auto& current=s.states[listKey_locked(self)];
      const bool tracked=current.capture_hooks;
      auto table=current.table;auto tables=current.tables;
      const auto gap=current.observation_gap;
      auto device=current.native_binding_device;
      const auto created=current.creation_observed;const auto path=current.registration_path;
      const auto reset=current.reset_observed||(observedReset&&tracked);
      const auto earlyReset=current.reset_before_capture_hooks||(observedReset&&!tracked);
      const auto interfaces=current.interfaces_observed;
      const auto cleanup=current.cleanup_established;
      if(!observedReset)if(auto* l=l3_list_locked(current)) {
          l->t10_vs_parameter=UINT32_MAX;l->t10_vs_table=false;l->vs_source={};
          l->sources.fill(diagnostics::L3Observation::Source{});l->t10_parameter=UINT32_MAX;
          l->t10_table=false;l->table_heap=nullptr;l->table_slot=nullptr;l->table_diagnostics=nullptr;
      }
      auto next=current.bindings; next.reset();next.tracking_complete=tracked&&(created||reset);
      oldPso.swap(current.pso);oldRoot.swap(current.root);oldHeaps.swap(current.heaps);
      (void)capture::new_lifetime_identity(); // former current=ListState{} temporary
      current.bindings=std::move(next);current.capture_hooks=tracked;current.table=table;
      if(auto* log=diagnostics::sink();log&&log->ownership_pins()) {
          current.constantWitness.reset(current.bindings.list_identity,current.bindings.generation);
      } else {
          current.constantWitness.~RootConstantWitness();
          new (&current.constantWitness) diagnostics::RootConstantWitness{};
      }
      current.tableWitness={};current.samplerWitness={};current.observation_gap=nullptr;
      current.native_binding_history=tracked;
      current.tables=tables;current.native_binding_device=device;
      // J-3b7: a Reset/ClearState starts from the D3D12 default RS/OM state.
      if(current.step3.rtPendingCounted&&state().step5RtPending>0)--state().step5RtPending; // reset unexecuted: its barriers never run
      current.step3=ListState::Step3Track{};current.step3.known=step3TablesHooked_locked(current);
      current.registration_path=path;current.creation_observed=created;current.reset_observed=reset;current.reset_before_capture_hooks=earlyReset;
      current.interfaces_observed=interfaces;
      current.cleanup_established=cleanup;
      if(!tracked)current.observation_gap=gap;
    }
}
ULONG STDMETHODCALLTYPE releaseMetadata(IUnknown* self);
ULONG STDMETHODCALLTYPE hookedRelease(ID3D12GraphicsCommandList* self) {
    HookAdmission admission;if(!admission)return 0;
    return releaseMetadata(reinterpret_cast<IUnknown*>(self));
}
// One owner retains slot 2's native original AND pending protection. Secondary
// registry membership never stores another Release original or chains hooks.
// The physical entry remains the first owner's handler throughout its lifetime.
Hooks* releaseHooks_locked(void** table,bool requireSlot=true) {
    auto& s=state();auto l=s.lists.find(table),m=s.objects.find(table);
    const bool list=l!=s.lists.end()&&l->second.originals[2];
    const bool metadata=m!=s.objects.end()&&m->second.originals[2];
    if(list==metadata)return nullptr; // absent or ambiguous ownership
    auto* hooks=list?&l->second:&m->second;
    const auto handler=list?reinterpret_cast<void*>(&hookedRelease):reinterpret_cast<void*>(&releaseMetadata);
    if(hooks->originals[2]==reinterpret_cast<void*>(&hookedRelease)||
       hooks->originals[2]==reinterpret_cast<void*>(&releaseMetadata))return nullptr;
    if(requireSlot&&table[2]!=handler&&table[2]!=hooks->originals[2])return nullptr;
    return hooks;
}
bool patchRelease_locked(void** table,void* handler,Hooks& owner) {
    if(owner.originals[2]) {
        if(releaseHooks_locked(table)!=&owner)return false;
    } else if(table[2]==reinterpret_cast<void*>(&hookedRelease)||
              table[2]==reinterpret_cast<void*>(&releaseMetadata))return false;
    return patch(table,2,handler,owner);
}
bool releaseCovered_locked(void** table) {
    if(!table)return false;auto* hooks=releaseHooks_locked(table);
    return hooks&&!hooks->pendingProtection[2]&&
        (table[2]==reinterpret_cast<void*>(&hookedRelease)||table[2]==reinterpret_cast<void*>(&releaseMetadata));
}
bool trackMetadataLifetime_locked(IUnknown* obj) {
    auto& s=state();auto table=vtable(obj);
    if(s.compatibility.count(table)||s.devices.count(table)||s.queues.count(table))return false;
    // Refuse an object already routed as a list; table sharing is allowed,
    // conflicting per-object ownership is not.
    if(s.listKeys.count(obj)||s.states.count(reinterpret_cast<ID3D12GraphicsCommandList*>(obj)))return false;
    auto& metadata=s.objects[table];
    auto l=s.lists.find(table);
    if(l!=s.lists.end()&&l->second.originals[2]) {
        auto* owner=releaseHooks_locked(table);if(!owner)return false;
        metadata.metadataRelease=true;
        return patchRelease_locked(table,reinterpret_cast<void*>(&hookedRelease),*owner);
    }
    metadata.metadataRelease=true;
    return patchRelease_locked(table,reinterpret_cast<void*>(&releaseMetadata),metadata);
}
// Teardown dispatches only an already-owned interface. Bypass our own Release
// observer (which refuses entry after closure); never QI/AddRef during cleanup.
void releaseOwned(IUnknown* value) noexcept {
    if(!value)return;
    using Fn=ULONG(STDMETHODCALLTYPE*)(IUnknown*);
    Fn fn=nullptr;
    {auto& s=state();std::lock_guard lock(s.mutex);
     if(auto* hooks=releaseHooks_locked(vtable(value),false))fn=reinterpret_cast<Fn>(hooks->originals[2]);
     else {
         for(auto* map:{&s.devices,&s.queues,&s.compatibility}) {
             auto it=map->find(vtable(value));
             if(it!=map->end()&&it->second.originals[2]){fn=reinterpret_cast<Fn>(it->second.originals[2]);break;}
         }
         if(!fn)fn=reinterpret_cast<Fn>(vtable(value)[2]);
     }}
    try {fn(value);}catch(...) {} // Unknown completion is never retried.
}
// Read-only snapshots under the existing mutex; no QI or patch attempts.
void releaseConditions_locked(diagnostics::LifetimeConditions& out,IUnknown* object,bool canonical) {
    if(!object)return;
    using namespace diagnostics;
    auto* table=vtable(object);auto& s=state();
    auto l=s.lists.find(table),m=s.objects.find(table);
    const bool pending=(l!=s.lists.end()&&l->second.pendingProtection[2])||
        (m!=s.objects.end()&&m->second.pendingProtection[2]);
    out.set(canonical?canonical_covered:base_covered,releaseCovered_locked(table));
    out.set(canonical?canonical_owner:base_owner,releaseHooks_locked(table)!=nullptr);
    out.set(canonical?canonical_pending:base_pending,pending);
}
#ifdef ACZ_VR_OFFLINE_TEST
bool g_copyObservationTest=false;
gpu_copy::OwnershipProvider g_copyOwnershipTest{};
#endif
#include "gpu_copy_step1_hooks.inl"
#include "gpu_copy_observation_hooks.inl"
#include "ui3d_census_hooks.inl"     // UI3D: ui3d-census.arm draw glue (read only; same adapters as the brief3d capture)
#include "brief3d_capture_hooks.inl" // BRIEF3D: brief3d-capture.arm draw glue (read only; needs step3MappedSpan_locked/guardedCpuRead above)
#include "brief3d_replay_hooks.inl"  // BRIEF3D live (brief3d:1): producer glue (draw replay, reset, execute/Signal); off: one relaxed load
#include "ui3d_replay_hooks.inl"     // UI3D stage 2 (ui3d:1): per-eye replay of the ps2_v menu draws on depth layers; off: one relaxed load
HRESULT STDMETHODCALLTYPE hookedReset(ID3D12GraphicsCommandList* self, ID3D12CommandAllocator* allocator, ID3D12PipelineState* pso) {
    HookAdmission admission;if(!admission)return E_FAIL;
    ConstantCall constantCall(self);
    CopyNativeCall copyObservation(self); // retain across native reentry/retirement
    HRESULT hr=original<PFN_Reset>(self, VTIDX_Reset)(self,allocator,pso);
    // Canonical route (certified alias map) for the ownership provider lookup.
    if(copyObservation.feed)copyObservation.feed->reset_result(copyObservation.key?copyObservation.key:self,allocator,SUCCEEDED(hr));
    if(g_step1Phase.load(std::memory_order_relaxed)>=static_cast<int>(Step1Phase::ready))
        observeSafely([&]{step1NoteReset(self,allocator,hr);});
    brief3dReset(self,hr); // BRIEF3D live: retirement witness of a replay recording (off: one relaxed load)
    fxAtlasListReset(self); // review 2026-11-01 F2: the compute census stops tracking this list (one relaxed compare-exchange)
    constantCall.returned();
    if(FAILED(hr)&&constantCall.enabled) {
        auto& s=state();std::lock_guard lock(s.mutex);auto it=s.states.find(listKey_locked(self));
        if(it!=s.states.end())it->second.constantWitness.fail(diagnostics::RootConstantWitness::history);
    }
    if (SUCCEEDED(hr)) {
        bool l3ResetRecorded=false;
        {auto& s=state();std::lock_guard lock(s.mutex);auto it=s.states.find(listKey_locked(self));
         if(it!=s.states.end())if(auto* l=l3_list_locked(it->second)) {
             const auto next=diagnostics::L3Observation::add(l->generation,1);
             l=s.l3.list(it->second.bindings.list_identity,next);diagnostics::L3Observation::inc(l->resets);l3ResetRecorded=true;
         }}
        try {
            bool unknown=false;
            {auto& s=state();std::lock_guard lock(s.mutex);auto it=s.states.find(listKey_locked(self));
             unknown=it==s.states.end()||!it->second.bindings.list_identity||!it->second.interfaces_observed;}
            // Only an already-patched Reset table can reach this seam. Certify
            // all aliases (including final Release) outside the state mutex.
            if(unknown) {
                if(!hookCaptureCommandList(self,diagnostics::ListSeam::reset)) {
                    {auto& s=state();std::lock_guard lock(s.mutex);
                     if(!s.states.count(listKey_locked(self)))return hr;}
                    // Foundation-only tables still observe the initial PSO.
                    // Discard the recording, preserve fail-closed certification.
                    resetBindings(self,true);setPSO(self,pso);
                    auto& s=state();std::lock_guard lock(s.mutex);
                    auto& failed=s.states[listKey_locked(self)];failed.bindings.list_identity=0;
                    // resetBindings preserves the certification refusal.
                    if(!failed.observation_gap)failed.observation_gap=diagnostics::list_refusal_reasons[static_cast<unsigned>(diagnostics::ListRefusal::exception)];
                    return hr;
                }
                auto& s=state();std::lock_guard lock(s.mutex);
                // Raw setter histories may have allocated a placeholder State.
                // Registration at Reset begins a fresh lifetime point.
                auto& current=s.states[listKey_locked(self)];
                current.bindings=capture::State{};current.registration_path=2;
                diagnostics::list_registered(2);
            }
            resetBindings(self,true);setPSO(self,pso);
            if(!l3ResetRecorded){auto& s=state();std::lock_guard lock(s.mutex);
             auto it=s.states.find(listKey_locked(self));if(it!=s.states.end())
                 if(auto* l=l3_list_locked(it->second))diagnostics::L3Observation::inc(l->resets);}
        }
        catch(...) {auto& s=state();std::lock_guard lock(s.mutex);auto it=s.states.find(listKey_locked(self));
            if(it!=s.states.end()){it->second.bindings.tracking_complete=false;
                it->second.bindings.tracking_gap="rejected:setter observation exception/allocation failure";}}
    }
    return hr;
}
void STDMETHODCALLTYPE hookedClearState(ID3D12GraphicsCommandList* self, ID3D12PipelineState* pso) {
    HookAdmission admission;if(!admission)return;
    CopyNativeCall copyObservation(self);
    ConstantCall constantCall(self);
    original<PFN_ClearState>(self, VTIDX_ClearState)(self,pso);
    copyObservation.event(gpu_copy::Event::clear_state);
    constantCall.returned();
    observeBinding(self,[&]{resetBindings(self);setPSO(self,pso);});
}
void STDMETHODCALLTYPE hookedSetPipelineState(ID3D12GraphicsCommandList* self, ID3D12PipelineState* pso) {
    HookAdmission admission;if(!admission)return;
    ConstantCall constantCall(self);
    original<PFN_SetPipelineState>(self, VTIDX_SetPipelineState)(self,pso);
    constantCall.returned();
    observeBinding(self,[&]{setPSO(self,pso);});
}
// Preconditions: HookState::mutex held. Public lock-taking wrappers below
// preserve their API; diagnostics never recursively acquires that mutex.
binding::NativeResolution resolveNativeTableBinding_locked(const capture::State&,uint32_t,capture::Id,uint32_t);
binding::NativeResolution resolveNativeRootBinding_locked(const capture::State&,uint32_t,uint32_t,uint32_t,uint32_t);
binding::Status currentNativeBinding_locked(const binding::NativeResolution&,const capture::State&);
const ListState* liveBindingState(const capture::State&);
uint64_t requirementsClock() noexcept {
    const auto ms=GetTickCount64();return ms>UINT64_MAX/1000?UINT64_MAX:ms*1000;
}
// Injected only by source-including offline fixtures. No runtime reset/enable API.
auto g_requirementsClock=&requirementsClock;
#ifdef ACZ_VR_OFFLINE_TEST
void (*g_requirementsProbe)()=nullptr;
#endif
binding::NativeResolution resolveNativeView_locked(const capture::State& live,const capture::BufferView& view) {
    // IA stores the observed GPU VA as an opaque token in BufferView::resource.
    // Adapt the explicit view byte count to the observer's existing root-SRV
    // address resolver (four-byte enclosing span). This is metadata arithmetic,
    // never a shader SRV assertion, dereference, GPU read or device call.
    if(!view.resource||!view.size||view.size>UINT32_MAX||view.offset>UINT64_MAX-view.resource)return {};
    capture::State adapter;
    adapter.list_identity=live.list_identity;adapter.generation=live.generation;adapter.revision=live.revision;
    adapter.tracking_complete=live.tracking_complete;
    const auto address=view.resource+view.offset;
    const auto leading=address%4;
    const auto words=(leading+view.size+3)/4; // size <= UINT32_MAX
    adapter.set_value(0,metadata::ParameterKind::srv,address-leading);
    adapter.revision=live.revision;
    const auto* point=liveBindingState(live);if(!point)return {};
    auto resolved=state().nativeBindings.root(adapter,0,0,static_cast<uint32_t>(words),4,point->native_binding_device);
    if(resolved) {
        // Retain current resource provenance and the EXACT view span. Metadata
        // only: not a byte copy. An enclosing span which exceeds the resource
        // conservatively rejects (e.g. a final unpadded 16-bit index).
        auto exact=std::make_shared<binding::Snapshot>(*resolved.snapshot);
        exact->descriptor.view.offset+=leading;exact->descriptor.view.size=view.size;
        resolved.snapshot=std::move(exact);
    }
    return resolved;
}
// Static first-failure reasons; called only under the capture-state mutex.
// Supplied copies must still identify THIS list, generation and revision.
const char* listPointFailure_locked(const ListState& list,const capture::State& supplied) noexcept {
    const auto& b=list.bindings;
    if(!supplied.list_identity||!b.list_identity)
        return list.observation_gap?list.observation_gap:"rejected:no list identity";
    if(supplied.list_identity!=b.list_identity)return "rejected:cross-list identity mismatch";
    if(!b.generation||supplied.generation!=b.generation)return "rejected:list generation mismatch";
    if(supplied.revision!=b.revision)return "rejected:list revision mismatch";
    if(list.observation_gap)return list.observation_gap;
    // A shared patched table can observe a late list's setters before queue
    // submission discovers/certifies that object. No certification attempt
    // has failed here; preserve the actual creation/Reset observation gap.
    // Explicit installer failures above still take precedence and fail closed.
    if(!list.interfaces_observed&&!list.creation_observed)
        return list.reset_before_capture_hooks?"rejected:first Reset predates complete hooks":"rejected:created before hook/creation unobserved; no observed Reset";
    if(!list.capture_hooks||!list.interfaces_observed)return "rejected:unhooked list vtable or partial installation";
    if(!list.creation_observed&&!list.reset_observed)
        return list.reset_before_capture_hooks?"rejected:first Reset predates complete hooks":"rejected:created before hook/creation unobserved; no observed Reset";
    if(!list.native_binding_history)return "rejected:recording history unobserved";
    if(!list.native_binding_device)return "rejected:list device identity unobserved";
    if(b.tracking_gap)return b.tracking_gap;
    return nullptr;
}
// Refine only the original checker gate that actually failed. An absent later
// component never hides a prior descriptor/root rejection. Root constants keep
// their explicit unsupported proof rejection even if no constants were written.
const char* missingComponent(const capture::State& b,const capture::Command& cmd,const char* outcome) {
    using namespace metadata;
    const std::string_view why=outcome?outcome:"";
    if(why=="rejected:PSO/root identity mismatch or unaccepted root"||!b.tracking_complete) {
        if(!b.pipeline.identity)return "rejected:missing PSO observation";
        if(!b.root_id||!b.root)return "rejected:missing root signature observation";
    }
    if(b.root) {
        auto missing=[&](BindingKind kind,uint32_t reg,ShaderStage stage) {
            auto binding=b.root->resolve(kind,reg,0,stage);
            return binding.valid&&binding.parameter_kind!=ParameterKind::constants&&
                !b.roots.count(binding.root_index);
        };
        if((why=="rejected:t10 missing/stale/unsupported descriptor"||!b.tracking_complete)&&
            (missing(BindingKind::srv,10,ShaderStage::vertex)||missing(BindingKind::srv,10,ShaderStage::geometry)))
            return "rejected:missing t10 root parameter observation";
        if((why=="rejected:cb10 missing/stale/unsupported descriptor"||!b.tracking_complete)&&
            (missing(BindingKind::cbv,10,ShaderStage::vertex)||missing(BindingKind::cbv,10,ShaderStage::pixel)))
            return "rejected:missing cb10 root parameter observation";
    }
    bool needsHeaps=false;
    if(b.root) {
        auto table=[&](BindingKind kind,uint32_t reg,ShaderStage stage) {
            auto binding=b.root->resolve(kind,reg,0,stage);
            return binding.valid&&binding.parameter_kind==ParameterKind::table;
        };
        if(why=="rejected:t10 missing/stale/unsupported descriptor")
            needsHeaps=table(BindingKind::srv,10,ShaderStage::vertex)||table(BindingKind::srv,10,ShaderStage::geometry);
        else if(why=="rejected:cb10 missing/stale/unsupported descriptor")
            needsHeaps=table(BindingKind::cbv,10,ShaderStage::vertex)||table(BindingKind::cbv,10,ShaderStage::pixel);
        else needsHeaps=why=="rejected:t0 missing/stale/unsupported descriptor"||why=="rejected:s0 missing/stale/unsupported descriptor";
    }
    if((needsHeaps||!b.tracking_complete)&&b.heaps.empty())return "rejected:missing descriptor heaps observation";
    if((why=="rejected:IA slot zero view/stride 64 required"||!b.tracking_complete)&&!b.vertices.count(0))
        return "rejected:missing IA vertex buffer observation";
    if((why=="rejected:index view missing/unsupported stride"||!b.tracking_complete)&&cmd.indexed&&!b.index)
        return "rejected:missing index buffer observation";
    if((why.find("metadata-proved")==0||!b.tracking_complete)&&!b.topology)
        return "rejected:missing topology observation";
    return !b.tracking_complete?"rejected:tracking incomplete; component observation lost":outcome;
}
// Extend the logger's legacy literal inventory here (its schema and partitions
// stay unchanged). Tags express equality/provenance only, never pointer bits,
// hashes of addresses, lifetime counters or arbitrary formatted reason keys.
constexpr const char* pipeline_identity_reasons[]={
    "rejected:PSO/root stale PSO observation;ps=ne",
    "rejected:PSO/root explicit lifetime unavailable;pc=split;pr=0;canonical-release=unproven",
    "rejected:PSO/root explicit lifetime unavailable;pc=unknown;pr=0",
    "rejected:PSO/root explicit lifetime unavailable;pi=unsupported;pr=0",
    "rejected:PSO/root lifetime mismatch;key=eq;life=ne",
    "rejected:PSO/root lifetime mismatch;key=ne;life=ne",
    "rejected:PSO/root lifetime mismatch;key=unknown;life=ne"
};
// Conservative extension of the legacy inventory, counting even replaced
// literals. Overflow remains explicit in the existing 32-outcome partition.
static_assert(std::size(pipeline_identity_reasons)==7&&std::size(requirements::metadata_identity_reasons)==7);
static_assert(diagnostics::metadata_static_outcome_count+std::size(pipeline_identity_reasons)+
    std::size(requirements::metadata_identity_reasons)==94);
static_assert([] {
    for(const auto* why:pipeline_identity_reasons)if(std::string_view(why).size()>=110)return false;
    for(const auto* why:requirements::metadata_identity_reasons)if(std::string_view(why).size()>=110)return false;
    return true;
}()); // Existing rejection formatter remains below its 225-character bound.
bool pinnedPipeline_locked(const PipelineRecord&,ID3D12PipelineState*) noexcept;
bool pinnedRoot_locked(const PipelineRecord&,ID3D12RootSignature*) noexcept;
const char* pipelineIdentityFailure_locked(const PipelineRecord& p,const capture::State& b) noexcept {
    if(p.identity!=b.pipeline.identity)return pipeline_identity_reasons[0];
    const auto* why=requirements::metadata_identity_failure(b);
    if(why==requirements::metadata_identity_reasons[4]) {
        if(!p.creation_interface_supported)return pipeline_identity_reasons[3];
        // D-119: split is a creation-time QI(IUnknown) inequality, not an
        // unaccepted root layout. Failed canonical validation/coverage or
        // ambiguous per-object routing still leaves the explicit token zero.
        if(p.canonical==PipelineRecord::Canonical::split)return pipeline_identity_reasons[1];
        if(p.canonical==PipelineRecord::Canonical::unavailable)return pipeline_identity_reasons[2];
    }
    if(why==requirements::metadata_identity_reasons[6]) {
        // The retained explicit root pins its interface. A missing alias here
        // says provenance is unavailable; do not compare raw interface bits
        // to the canonical key or recover a lifetime from a different object.
        const auto k=state().rootKeys.find(p.root.get());
        if(k==state().rootKeys.end())return pipeline_identity_reasons[6];
        return reinterpret_cast<uintptr_t>(k->second)==b.root_id?
            pipeline_identity_reasons[4]:pipeline_identity_reasons[5];
    }
    return why;
}
using T0Diagnostics=diagnostics::DescriptorTableDiagnostics;
// Caller-owned bounded snapshot; no layout/heap/resource scan or COM here.
void t0Snapshot_locked(const ListState& list,const PipelineRecord& pipeline,
                       unsigned descriptorBefore,T0Diagnostics::Snapshot& tableSnapshot,bool sampler=false) noexcept {
    auto& s=state();const auto& b=list.bindings;
    using D=diagnostics::DescriptorTableDiagnostics;
    const auto& w=sampler?list.samplerWitness:list.tableWitness;
    tableSnapshot=s.nativeBindings.diagnostic_snapshot(w.location,w.offset,sampler?D::sampler:D::srv);
    tableSnapshot.parameter=w.parameter;
    const auto table=b.roots.find(w.parameter);
    if(table==b.roots.end()||table->second.kind!=metadata::ParameterKind::table)
        tableSnapshot.flags|=D::no_table;
    if(!w.covered(list.constantWitness,b.list_identity,b.generation,b.root_id,b.root_identity,
        GetCurrentThreadId(),!listPointFailure_locked(list,b)&&b.tracking_complete&&
        b.pipeline.identity==pipeline.identity&&b.root_identity==pipeline.root_identity)||
        table==b.roots.end()||table->second.value!=w.handle)
        tableSnapshot.flags|=D::uncovered;
    if(!b.tracking_complete||s.bindingEvents.load())tableSnapshot.flags|=D::incomplete;
    tableSnapshot.classify(w.descriptor_before,w.descriptor_after,descriptorBefore,
        !(tableSnapshot.flags&(D::no_table|D::uncovered)));
}
unsigned t0Ownership_locked(const T0Diagnostics::Snapshot& value) noexcept {
    const auto candidate=value.ownership_reason();
    if(candidate!=T0Diagnostics::pin_owned)return candidate;
    unsigned count=0;bool free=false;
    for(const auto& pin:state().ownershipPins) {
        if(!pin.reserved)free=true;
        if(!pin.texture)continue;
        ++count;
        if(pin.textureKey==SrvCandidates::Key{value.view_write.address,value.heap,value.ordinal,value.slot})
            return pin.complete?T0Diagnostics::pin_owned:pin.textureReason;
    }
    if(value.slot>=SrvCandidates::slots_per_heap)return T0Diagnostics::pin_candidate_slot_cap;
    return count>=4||!free?T0Diagnostics::pin_cap:T0Diagnostics::pin_candidate_missing;
}
// Draw never calls COM through a historical resource address. It only transfers
// the already-owned reference acquired during the actual SRV API call.
void promoteT0AtDraw(void* self,unsigned descriptorBefore) noexcept {
    auto* log=diagnostics::sink();if(!log||!log->ownership_pins())return; // U-soak: pins are runtime state; never gated on shared-log I/O health
    auto& s=state();std::lock_guard lock(s.mutex);
    auto it=s.states.find(listKey_locked(self));if(it==s.states.end())return;
    const auto& list=it->second;const auto& b=list.bindings;
    const auto p=s.pipelines.find(list.pso.get());
    if(p==s.pipelines.end()||!p->second.drawcpt_identity_matched||
       listPointFailure_locked(list,b)||!b.tracking_complete||!s.nativeBindings.enabled()||
       s.bindingEvents.load()||s.rootCreates||s.metadataReleases||
       pipelineIdentityFailure_locked(p->second,b)||
       !pinnedPipeline_locked(p->second,list.pso.get())||!pinnedRoot_locked(p->second,list.root.get()))return;
    T0Diagnostics::Snapshot value;
    t0Snapshot_locked(list,p->second,descriptorBefore,value);
    if(value.ownership_reason()!=T0Diagnostics::pin_owned)return;
    const SrvCandidates::Key key{value.view_write.address,value.heap,value.ordinal,value.slot};
    unsigned count=0;size_t slot=s.ownershipPins.size();
    for(size_t i=0;i<s.ownershipPins.size();++i) {
        const auto& pin=s.ownershipPins[i];
        if(!pin.reserved&&slot==s.ownershipPins.size())slot=i;
        if(pin.texture){++count;if(pin.textureKey==key)return;}
    }
    if(count>=4||slot==s.ownershipPins.size()){SrvCandidates::increment(s.srvCandidates.refused);return;}
    auto* owned=s.srvCandidates.promote(key);
    srvCandidateCounts_locked();
    if(!owned)return; // evicted/replaced/pending/unobserved: distinct op=a
    auto& pin=s.ownershipPins[slot];pin.reserved=true;pin.texture=owned;
    pin.textureKey=key;pin.counts[0]=1;pin.complete=true;pin.textureReason=T0Diagnostics::pin_owned;
}
struct T0MetadataContext {
    const T0Diagnostics::Snapshot* snapshot=nullptr;
    const T0Diagnostics::Snapshot* sampler=nullptr;
    void* list=nullptr;
};
bool cpuRequestCurrent_locked(void*,const diagnostics::CpuUploadSnapshot::Request&) noexcept;
void observeRequirements_locked(ListState& list,const capture::Command& cmd,unsigned descriptorBefore=UINT32_MAX,diagnostics::CpuUploadSnapshot::Request* cpu=nullptr,
    const diagnostics::CpuUploadSnapshot::Request* evaluated=nullptr,void* self=nullptr) noexcept {
    auto& s=state();auto* log=diagnostics::sink();
    if(!log||!log->enabled()||(!log->ownership_pins()&&s.requirements_attempts>=requirements_attempt_limit))return;
    const auto p=s.pipelines.find(list.pso.get());
    if(p==s.pipelines.end()||!p->second.drawcpt_identity_matched)return;
    auto& scratch=s.requirementsScratch;
    // Reconstruct directly in fixed state storage; aggregate assignment would
    // permit a large zero-initialized temporary on the hook stack.
    scratch.~RequirementsScratch();new (&scratch) HookState::RequirementsScratch{};
    auto& constantSnapshot=scratch.constants;
    auto& tableSnapshot=scratch.table;auto& samplerSnapshot=scratch.sampler;
    if(log->ownership_pins()) {
        const auto& b=list.bindings;
        constantSnapshot=list.constantWitness.snapshot(b.list_identity,b.generation,b.root_id,b.root_identity,
            GetCurrentThreadId(),!listPointFailure_locked(list,b)&&b.tracking_complete&&b.pipeline.identity==p->second.identity&&
            b.root_identity==p->second.root_identity);
        constantSnapshot.binding_revision=b.revision;
        if(!evaluated)s.constantWitnessCounts.record(constantSnapshot);
        t0Snapshot_locked(list,p->second,descriptorBefore,tableSnapshot);
        t0Snapshot_locked(list,p->second,descriptorBefore,samplerSnapshot,true);
        if(!evaluated)s.tableWitnessCounts.record(tableSnapshot.flags);
    }
    if(!evaluated&&s.requirements_attempts>=requirements_attempt_limit)return;
    const auto now=g_requirementsClock();
    if(!evaluated&&s.requirements_attempts&&(now<s.requirements_last_us||now-s.requirements_last_us<requirements_interval_us))return;
    if(!evaluated){s.requirements_last_us=now;++s.requirements_attempts;}
    auto& result=scratch.result;
    try {
#ifdef ACZ_VR_OFFLINE_TEST
        if(g_requirementsProbe)g_requirementsProbe();
#endif
        const auto& b=list.bindings;
        if(const auto* gap=listPointFailure_locked(list,b))result.outcome=gap;
        else if(!b.tracking_complete)result.outcome=missingComponent(b,cmd,nullptr);
        else if(p->second.identity!=b.pipeline.identity)result.outcome=pipeline_identity_reasons[0];
        else if(!s.nativeBindings.enabled())result.outcome="rejected:native binding observer disabled";
        else if(s.bindingEvents.load()||s.rootCreates||s.metadataReleases)
            result.outcome="rejected:busy native capture point";
        else if(const auto* why=pipelineIdentityFailure_locked(p->second,b))result.outcome=why;
        else if((p->second.ownershipPin||p->second.ownershipRequired)&&(!pinnedPipeline_locked(p->second,list.pso.get())||
            !pinnedRoot_locked(p->second,list.root.get())))result.outcome=pipeline_identity_reasons[1];
        else {
            T0MetadataContext t0Context{&tableSnapshot,&samplerSnapshot,self};
            const requirements::MetadataResolver resolver{&t0Context,
                [](void*,const capture::State& b,uint32_t p,capture::Id h,uint32_t o){return resolveNativeTableBinding_locked(b,p,h,o);},
                [](void*,const capture::State& b,uint32_t p,uint32_t n,uint32_t e,uint32_t stride){return resolveNativeRootBinding_locked(b,p,n,e,stride);},
                [](void*,const capture::State& b,const capture::BufferView& v){return resolveNativeView_locked(b,v);},
                [](void*,const capture::State& b,const binding::NativeResolution& v){return bool(currentNativeBinding_locked(v,b));},
                p->second.drawcpt_ia_layout,
                log->ownership_pins()?&constantSnapshot:nullptr,
                p->second.vs_cb10_words,p->second.ps_cb10_words,
                log->ownership_pins()?GetCurrentThreadId():0u,
                log->ownership_pins()?+[](void* context,const capture::State&,uint32_t parameter,uint32_t offset) {
                    const auto& t=*static_cast<T0MetadataContext*>(context)->snapshot;
                    return t.parameter==parameter&&t.offset==offset&&t0Ownership_locked(t)==T0Diagnostics::pin_owned;
                }:nullptr,
                log->ownership_pins()?+[](void* context,const capture::State&,uint32_t parameter,uint32_t offset) {
                    const auto& t=*static_cast<T0MetadataContext*>(context)->sampler;
                    return t.parameter==parameter&&t.offset==offset&&t.sampler_reason()==T0Diagnostics::Snapshot::sampler_covered;
                }:nullptr,
                log->ownership_pins()&&evaluated?&s.cpuSnapshot:nullptr,
                log->ownership_pins()?evaluated:nullptr,
                log->ownership_pins()&&evaluated?+[](void* context,const diagnostics::CpuUploadSnapshot::Request& q) {
                    return cpuRequestCurrent_locked(static_cast<T0MetadataContext*>(context)->list,q);
                }:nullptr};
            requirements::check_metadata(b,cmd,resolver,result);
            result.outcome=missingComponent(b,cmd,result.outcome);
            if(std::string_view(result.outcome).find("metadata-proved")!=0)result.proved=false;
            if(s.bindingEvents.load()) {result.proved=false;result.outcome="rejected:busy native capture point";}
        }
    } catch(...) {result.proved=false;result.constants_covered=false;
        result.outcome="rejected:metadata exception/allocation failure";}
    if(cpu&&log->ownership_pins()&&result.proved&&cmd.indexed&&s.cpuSnapshot.attempts<diagnostics::CpuUploadSnapshot::limit) {
        const auto& b=list.bindings;
        auto* l=l3_list_locked(list);
        cpu->list=b.list_identity;cpu->generation=b.generation;cpu->revision=b.revision;cpu->draw=l?l->draws:0;
        cpu->observation_epoch=bindingCohort().clock;
        cpu->vertex_stride=result.ia_stride;cpu->index_stride=b.index?b.index->stride:0;
        cpu->start=cmd.start;cpu->count=cmd.count;cpu->base=cmd.base_vertex;
        cpu->instances=cmd.instances;cpu->start_instance=cmd.start_instance;
        cpu->topology=b.topology;cpu->strip_cut=b.pipeline.strip_cut;cpu->descriptor_before=descriptorBefore;
        cpu->accepted=l&&cpu->draw&&pinnedPipeline_locked(p->second,list.pso.get())&&pinnedRoot_locked(p->second,list.root.get());
        for(size_t i=0;i<3;++i) {
            auto& x=cpu->ranges[i];x.resource=result.cpu_resources[i].id;x.lifetime=result.cpu_resources[i].generation;
            x.offset=result.cpu_offsets[i];x.bytes=result.cpu_sizes[i];
            const auto* r=s.l3.resources.get(x.resource);
            if(r) {
                x.map_generation=r->map_generation;x.mapped=r->mapped;x.resource_bytes=r->desc.Width;
                x.va=r->gpu_va&&x.offset<=UINT64_MAX-r->gpu_va?r->gpu_va+x.offset:0;
                if(r->retired||r->map_gap||!r->serial||r->mapped_subresource!=0||r->mapped_lifetime!=x.lifetime||r->outstanding!=1||r->map_inflight)x.mapped=nullptr;
                x.upload=r->description&&r->kind==1&&r->heap==D3D12_HEAP_TYPE_UPLOAD&&r->desc.Dimension==D3D12_RESOURCE_DIMENSION_BUFFER;
            }
            if(!x.resource||!x.lifetime)cpu->accepted=false;
        }
        cpu->accepted=s.cpuSnapshot.reserve(log->ownership_pins(),*cpu);
    }
    // Reuse the checker's existing cb10 resolution. Counts sampled attempts,
    // not all draws, and never supplies an eligibility fact.
    if(!evaluated&&log->ownership_pins()&&result.cb10_kind==static_cast<uint32_t>(metadata::ParameterKind::constants)&&
       s.cb10ConstantDraws<65535)++s.cb10ConstantDraws;
    // Fixed stack fields; no pointers, QI, allocation, I/O or new reason keys.
    // Preserve the legacy prefix, cb10, draw and lp fields byte-for-byte.
    auto& context=scratch.context;
    if(std::string_view(result.outcome).find("rejected:PSO/root explicit lifetime unavailable")==0) {
        diagnostics::LifetimeConditions bound;
        auto root=s.rootCreations.find(reinterpret_cast<ID3D12RootSignature*>(list.bindings.root_id));
        if(root!=s.rootCreations.end()) {
            bound=root->second.conditions;
            if(p->second.publication&&root->second.publication)
                bound.set(diagnostics::dependency_before,root->second.publication<p->second.publication);
        }
        const auto& pso=p->second;
        std::snprintf(context,sizeof context,";pd=%06x/%06x;rd=%06x/%06x;bd=%06x/%06x%s",
            pso.conditions.known,pso.conditions.value,pso.dependency_conditions.known,
            pso.dependency_conditions.value,bound.known,bound.value,pso.detail.data());
    }
    if(log->ownership_pins()) {
        const bool pinned=pinnedPipeline_locked(p->second,list.pso.get())&&pinnedRoot_locked(p->second,list.root.get())&&
            list.bindings.pipeline.identity==p->second.identity&&
            list.bindings.root_identity==p->second.root_identity;
        const auto n=std::strlen(context);
        std::snprintf(context+n,sizeof(context)-n,";pt=%04x;px=%04x;pb=%u",s.pinsTaken,s.pinsCapRefused,unsigned(pinned));
        // Constants results have no long lifetime context. Preserve its ceiling.
        if(result.cb10_kind==static_cast<uint32_t>(metadata::ParameterKind::constants)) {
            const auto used=std::strlen(context);
            std::snprintf(context+used,sizeof(context)-used,";cr=%04x;cw=%04x",s.cb10ConstantDraws,s.constantWordCalls);
            constantSnapshot.append(context,sizeof context);
            if(std::string_view(result.outcome)=="rejected:evidence cb10 resolves to root constants; descriptor-backed proof unsupported")
                s.constantWitnessCounts.append(context,sizeof context);
            if(result.constants_covered) {
                const auto used=std::strlen(context);
                // Keep the literal outcome as the exact next refusal. cn is a
                // bounded category, never a new reason key or formatted address.
                const std::string_view why=result.outcome;
                const char* next=result.proved?(why.find("index-range-unverified")!=why.npos?"index":"none"):why.find("t0")!=why.npos?"t0":
                    why.find("s0")!=why.npos?"s0":why.find("index")!=why.npos?"index":
                    why.find("IA")!=why.npos?"ia":why.find("draw")!=why.npos?"draw":"other";
                std::snprintf(context+used,sizeof(context)-used,";cs=covered;cn=%s",next);
                // This path has exactly two words and no wc/wv suffix. Largest
                // success (stride=416, ia=64) <=318 +100 context +139 overhead.
                static_assert(318+100<diagnostics::text_capacity&&318+100+139<=diagnostics::max_record_length);
            }
            // Exact refusal: <=161 legacy chars +202 context +54 counters.
            // Serialized record overhead <=139, so 556 <=576; context <=256.
            static_assert(161+202+54<diagnostics::text_capacity&&161+202+54+139<=diagnostics::max_record_length);
        }
    }
    if(log->ownership_pins()&&std::string_view(result.outcome).find("rejected:t0")==0) {
        tableSnapshot.append(context,sizeof context);
        tableSnapshot.append_last(context,sizeof context);
        // 213 old context +40 new suffix; 161 legacy +253 context +139 overhead.
        static_assert(213+40<sizeof context&&161+253<diagnostics::text_capacity&&161+253+139<=diagnostics::max_record_length);
    }
    // One bounded sampled companion supplies sticky flags and all subreason
    // counts without consuming space in the legacy refusal's first example.
    if(!evaluated&&log->ownership_pins()) {
        auto& diagnostic=scratch.diagnostic;
        std::snprintf(diagnostic,sizeof diagnostic,"root-table-witness;draw=%u,%u,%u,%u,%d,%u",
            unsigned(cmd.indexed),cmd.count,cmd.instances,cmd.start,cmd.base_vertex,cmd.start_instance);
        tableSnapshot.append(diagnostic,sizeof diagnostic);
        s.tableWitnessCounts.append(diagnostic,sizeof diagnostic);
        {const auto n=std::strlen(diagnostic);
         std::snprintf(diagnostic+n,sizeof diagnostic-n,";op=%x",t0Ownership_locked(tableSnapshot));}
        static_assert(280+5<diagnostics::text_capacity&&280+5+139<=diagnostics::max_record_length);
        diagnostics::emit(diagnostics::record(diagnostics::Event::requirements_metadata,0,0,0,diagnostic));
        std::snprintf(diagnostic,sizeof diagnostic,"s0-table-witness;draw=%u,%u,%u,%u,%d,%u",
            unsigned(cmd.indexed),cmd.count,cmd.instances,cmd.start,cmd.base_vertex,cmd.start_instance);
        samplerSnapshot.append(diagnostic,sizeof diagnostic);
        samplerSnapshot.append_write(diagnostic,sizeof diagnostic);
        diagnostics::emit(diagnostics::record(diagnostics::Event::requirements_metadata,0,0,0,diagnostic));
        std::snprintf(diagnostic,sizeof diagnostic,"s0-description;draw=%u,%u,%u,%u,%d,%u",
            unsigned(cmd.indexed),cmd.count,cmd.instances,cmd.start,cmd.base_vertex,cmd.start_instance);
        if(list.bindings.root&&list.bindings.root->serialized_ps_s0()) {
            auto& sampler=samplerSnapshot;
            sampler.sampler_words=list.bindings.root->serialized_ps_s0();sampler.sampler_observed=true;sampler.sampler_ordinal=0;
            // Static facts are immutable root bytes; no descriptor/heap ordinal.
            const auto n=std::strlen(diagnostic);std::snprintf(diagnostic+n,sizeof diagnostic-n,";static=1");
        }
        samplerSnapshot.append_sampler(diagnostic,sizeof diagnostic,
            list.bindings.root&&list.bindings.root->serialized_ps_s0());
        diagnostics::emit(diagnostics::record(diagnostics::Event::requirements_metadata,0,0,0,diagnostic));
        // Resource companions share 64 samples (32 complete pairs); the write
        // witness retains its existing cap. All ring/disk limits are unchanged.
        // Emit write-time facts first so J-126's first-kind evidence reservation
        // retains them. The unchanged historical companion follows in the ring.
        std::snprintf(diagnostic,sizeof diagnostic,"t0-resource-description;view-write;draw=%u,%u,%u,%u,%d,%u",
            unsigned(cmd.indexed),cmd.count,cmd.instances,cmd.start,cmd.base_vertex,cmd.start_instance);
        tableSnapshot.view_write.append(diagnostic,sizeof diagnostic);
        diagnostics::emit(diagnostics::record(diagnostics::Event::requirements_metadata,0,0,0,diagnostic));
        std::snprintf(diagnostic,sizeof diagnostic,"t0-resource-description;draw=%u,%u,%u,%u,%d,%u",
            unsigned(cmd.indexed),cmd.count,cmd.instances,cmd.start,cmd.base_vertex,cmd.start_instance);
        tableSnapshot.texture.append(diagnostic,sizeof diagnostic);
        s.nativeBindings.append_texture_diagnostics(diagnostic,sizeof diagnostic);
        diagnostics::emit(diagnostics::record(diagnostics::Event::requirements_metadata,0,0,0,diagnostic));
        std::snprintf(diagnostic,sizeof diagnostic,"t0-write-witness;draw=%u,%u,%u,%u,%d,%u",
            unsigned(cmd.indexed),cmd.count,cmd.instances,cmd.start,cmd.base_vertex,cmd.start_instance);
        tableSnapshot.append_write(diagnostic,sizeof diagnostic);
        if(const auto* d=s.nativeBindings.diagnostic())d->append_creations(diagnostic,sizeof diagnostic);
        s.srvCandidates.append(diagnostic,sizeof diagnostic);
        srvCandidateCounts_locked();
        // Worst-width draw prefix + write fields + creation counts + candidates.
        static_assert(79+220+54+32<diagnostics::text_capacity&&
                      79+220+54+32+139<=diagnostics::max_record_length);
        diagnostics::emit(diagnostics::record(diagnostics::Event::requirements_metadata,0,0,0,diagnostic));
    }
    // Earlier requirements failures (bundle/indirect/history/lifetime) do not
    // resolve cb10. A bounded companion keeps their witness visible without
    // truncating or replacing their existing, potentially full context.
    if(!evaluated&&log->ownership_pins()&&result.cb10_kind!=static_cast<uint32_t>(metadata::ParameterKind::constants)) {
        char diagnostic[diagnostics::text_capacity]{};
        std::snprintf(diagnostic,sizeof diagnostic,"root-constant-witness;draw=%u,%u,%u,%u,%d,%u",
            unsigned(cmd.indexed),cmd.count,cmd.instances,cmd.start,cmd.base_vertex,cmd.start_instance);
        constantSnapshot.append(diagnostic,sizeof diagnostic);
        s.constantWitnessCounts.append(diagnostic,sizeof diagnostic);
        diagnostics::emit(diagnostics::record(diagnostics::Event::requirements_metadata,0,0,0,diagnostic));
    }
    diagnostics::metadata_outcome(result,cmd,list.registration_path,context);
}
void finishCpuSnapshot(void*,diagnostics::CpuUploadSnapshot::Request&) noexcept;
// Delayed diagnostic publication validates the complete native Draw interval.
// The legacy switch-OFF observation point remains unchanged.
void ConstantCall::drawReturned(const capture::Command& cmd) noexcept {
    returned();if(!enabled)return;
    auto& s=state();
    {std::lock_guard lock(s.mutex);
     auto it=s.states.find(listKey_locked(self));if(it==s.states.end())return;
     const auto& b=it->second.bindings;
     if(b.list_identity!=call.list||b.generation!=call.generation||b.revision!=bindingRevision)
         it->second.constantWitness.fail(diagnostics::RootConstantWitness::stale);}
    promoteT0AtDraw(self,descriptorBefore);
    diagnostics::CpuUploadSnapshot::Request cpu;
    {std::lock_guard lock(s.mutex);
     auto it=s.states.find(listKey_locked(self));if(it==s.states.end())return;
     auto& list=it->second;
     if(list.bindings.list_identity!=call.list||list.bindings.generation!=call.generation||
        list.bindings.revision!=bindingRevision)list.constantWitness.fail(diagnostics::RootConstantWitness::stale);
     observeRequirements_locked(list,cmd,descriptorBefore,&cpu);}
    if(cpu.accepted)finishCpuSnapshot(self,cpu);
}
void observeDraw(ID3D12GraphicsCommandList* self, UINT count, UINT instances, bool indexed, UINT start, INT base, UINT startInstance) {
    DrawCallback cb;
    std::shared_ptr<ID3D12PipelineState> pso;
    bool gs=false;
    BindingCallback bindingCb;
    std::shared_ptr<const capture::State> bindings;
    const bool briefing=acz::brobs::armed(); // BRIEFING-3D: off = this one relaxed load
    const bool b3cap=acz::b3cap::sampling(); // BRIEF3D capture: off = this one relaxed load
    const bool u3c=acz::u3c::sampling();     // UI3D census: off = this one relaxed load
    uint32_t brVs8=0,brPs8=0;uint8_t brTopo=0;
    {
        auto& s=state(); std::lock_guard lock(s.mutex);
        cb=s.drawCb;
        auto it=s.states.find(listKey_locked(self));
        if(briefing&&it!=s.states.end()) {
            const auto p=s.pipelines.find(it->second.pso.get());
            if(p!=s.pipelines.end()){brVs8=p->second.brVs8;brPs8=p->second.brPs8;brTopo=uint8_t(p->second.topology_type);}
        }
        if(b3cap&&it!=s.states.end())if(const auto p=s.pipelines.find(it->second.pso.get());p!=s.pipelines.end())
            b3capDraw_locked(it->second,p->second,indexed,count,instances); // BRIEF3D capture: read only, one sampled frame per second
        if(u3c&&it!=s.states.end())if(const auto p=s.pipelines.find(it->second.pso.get());p!=s.pipelines.end()) {
            const auto g=s.gs.find(it->second.pso.get());
            u3cDraw_locked(it->second,p->second,g!=s.gs.end()&&g->second,indexed,start,base,count,instances); // UI3D census: read only, one sampled frame per second
        }
        if(it!=s.states.end()) {
            if(auto* l=l3_list_locked(it->second)) {
                const auto matched=s.pipelines.find(it->second.pso.get());
                if(matched!=s.pipelines.end()&&matched->second.drawcpt_identity_matched) {
                    if(l->t10_table&&(!l->table_identity||l->table_identity!=s.nativeBindings.diagnostic_identity())) {
                        l->table_slot=nullptr;l->table_heap=nullptr;l->table_diagnostics=nullptr;l->gap=true;
                    }
                    s.l3.draw(*l,indexed);l3_sources_locked(it->second,*l);
                }
            }
            auto* log=diagnostics::sink();
            if(!log||!log->ownership_pins())observeRequirements_locked(it->second,{indexed,count,instances,start,base,startInstance});
        }
        if(it!=s.states.end()) pso=it->second.pso;
        auto known=s.gs.find(pso.get());
        gs=known!=s.gs.end() && known->second;
        bindingCb=s.bindingCb;
        if(bindingCb && it!=s.states.end()) {
            auto copy=it->second.bindings;
            // A creation/retirement in flight has no established observation order.
            if(s.rootCreates || s.metadataReleases) copy.root_identity=0;
            bindings=std::make_shared<const capture::State>(std::move(copy));
        }
    }
    if(briefing)acz::brobs::draw(brVs8,brPs8,brTopo,indexed,gs,count,instances); // BRIEFING-3D: own mutex, after the hook-state lock is released
    notify(cb, DrawCallInfo{self,pso.get(),gs,count,instances,indexed,start,base,startInstance});
    if(bindings) notify(bindingCb,std::move(bindings),capture::Command{indexed,count,instances,start,base,startInstance});
}
void STDMETHODCALLTYPE hookedDrawInstanced(ID3D12GraphicsCommandList* self, UINT count, UINT instances, UINT start, UINT startInstance) {
    HookAdmission admission;if(!admission)return;
    Step3OmPin omPin;
    ConstantCall constantCall(self);
    auto fn=original<PFN_DrawInstanced>(self,VTIDX_DrawInstanced);
    uint64_t t0=perf::ticks(); // FPS investigation: per-section cost (observe only)
    observeSafely([&]{observeDraw(self,count,instances,false,start,0,startInstance);});
    uint64_t t1=perf::ticks();perf::add(perf::draw_observe,t1-t0);
    // Step-1 experiment (default OFF: phase 0): at most one proxy copy recorded
    // immediately before this original Draw, which is forwarded unchanged once.
    if(const int phase=g_step1Phase.load(std::memory_order_relaxed);phase==static_cast<int>(Step1Phase::ready))
        step1Insert(self,step1Pack(count,instances),step1Pack(start,0),step1Pack(startInstance,0));
    else if(phase==static_cast<int>(Step1Phase::inserted)&&g_step3SetActive.load(std::memory_order_relaxed))
        step3SetMember(self,step1Pack(count,instances),step1Pack(start,0),step1Pack(startInstance,0)); // J-3b7f later set members
    else if(phase==static_cast<int>(Step1Phase::continuous)) {
        step5Draw(self,step1Pack(count,instances),step1Pack(start,0),step1Pack(startInstance,0)); // J-3b8 continuous producer
        step5HudDraw(self,step1Pack(count,instances),step1Pack(start,0),step1Pack(startInstance,0)); // J-hud-capture (default OFF)
    }
    t0=perf::ticks();perf::add(perf::draw_step5,t0-t1);
    hudObserveDraw(self,count,instances,false);
    phaseObserveDraw(self,count,instances,false); // J-phase-observe (default OFF)
    ui3dDraw(self,count,instances,start,startInstance,false); // UI3D: per-eye replay of a ps2_v menu draw (off: one relaxed load)
    t1=perf::ticks();perf::add(perf::draw_other,t1-t0);
    fn(self,count,instances,start,startInstance);
    t0=perf::ticks();perf::add(perf::draw_native,t0-t1);
    constantCall.drawReturned({false,count,instances,start,0,startInstance});
    perf::add_time(perf::draw_observe,perf::ticks()-t0);
}
void STDMETHODCALLTYPE hookedDrawIndexedInstanced(ID3D12GraphicsCommandList* self, UINT count, UINT instances, UINT start, INT base, UINT startInstance) {
    HookAdmission admission;if(!admission)return;
    Step3OmPin omPin;
    ConstantCall constantCall(self);
    auto fn=original<PFN_DrawIndexedInstanced>(self,VTIDX_DrawIndexedInstanced);
    uint64_t t0=perf::ticks(); // FPS investigation: per-section cost (observe only)
    observeSafely([&]{observeDraw(self,count,instances,true,start,base,startInstance);});
    uint64_t t1=perf::ticks();perf::add(perf::draw_observe,t1-t0);
    if(const int phase=g_step1Phase.load(std::memory_order_relaxed);phase==static_cast<int>(Step1Phase::ready))
        step1Insert(self,step1Pack(count,instances),step1Pack(start,uint32_t(base)),step1Pack(startInstance,1));
    else if(phase==static_cast<int>(Step1Phase::inserted)&&g_step3SetActive.load(std::memory_order_relaxed))
        step3SetMember(self,step1Pack(count,instances),step1Pack(start,uint32_t(base)),step1Pack(startInstance,1));
    else if(phase==static_cast<int>(Step1Phase::continuous)) {
        step5Draw(self,step1Pack(count,instances),step1Pack(start,uint32_t(base)),step1Pack(startInstance,1));
        step5HudDraw(self,step1Pack(count,instances),step1Pack(start,uint32_t(base)),step1Pack(startInstance,1)); // J-hud-capture (default OFF)
    }
    t0=perf::ticks();perf::add(perf::draw_step5,t0-t1);
    hudObserveDraw(self,count,instances,true);
    phaseObserveDraw(self,count,instances,true);  // J-phase-observe (default OFF)
    t1=perf::ticks();perf::add(perf::draw_other,t1-t0);
    ui3dDraw(self,count,instances,start,startInstance,true,base); // UI3D: a non-ps2_v / indexed draw refuses the menu frame; u3a: the hangar ACM replay (off: one relaxed load)
    brief3dDraw(self,count,instances,start,base,startInstance); // BRIEF3D live: per-eye replay of a briefing map draw (off: one relaxed load)
    fn(self,count,instances,start,base,startInstance);
    t0=perf::ticks();perf::add(perf::draw_native,t0-t1);
    constantCall.drawReturned({true,count,instances,start,base,startInstance});
    perf::add_time(perf::draw_observe,perf::ticks()-t0);
}
void STDMETHODCALLTYPE hookedExecuteCommandLists(ID3D12CommandQueue* self, UINT count, ID3D12CommandList* const* lists) {
    HookAdmission admission;if(!admission)return;
    auto fn=original<PFN_ExecuteCommandLists>(self,VTIDX_ExecuteCommandLists,true);
    uint64_t perfT0=perf::ticks(); // FPS investigation: hook cost excluding the native call (observe only)
    ee_camera::notify_execute(); // J-w1-ee-camera seam C (one atomic load when OFF)
    diagnostics::execute_interval(); // Submission intervals, never Present/display frames.
    observeSafely([&] {
    if (!g_callbackDepth && lists) for(UINT i=0;i<count;++i) {
        if(!lists[i]) continue;
        ID3D12GraphicsCommandList* graphics=nullptr;
        if(SUCCEEDED(lists[i]->QueryInterface(IID_PPV_ARGS(&graphics)))) {
            std::unique_ptr<ID3D12GraphicsCommandList,ReleaseCOM> owned(graphics);
            if (!hookCommandList(graphics))
                OutputDebugStringA("[ACZ-VR] command-list hook incomplete\n");
#ifdef ACZ_VR_OFFLINE_TEST
            if(g_captureDiscoveryTest)
#endif
            {perf::Scope cert(perf::list_cert); // FPS growth investigation (observe only)
             hookCaptureCommandList(graphics,diagnostics::ListSeam::execute);} // Cannot recover prior recording.
        }
    }
    ExecuteCallback cb;
    {auto& s=state(); std::lock_guard lock(s.mutex); cb=s.executeCb;perf::gauge(perf::list_states,s.states.size());}
    notify(cb,self,count,lists);
    });
    observeSafely([&] {
        // Join the generation at call entry. Native reentry may Reset/release
        // a list during forwarding; a post-return lookup would mislabel it.
        auto& s=state();std::lock_guard lock(s.mutex);auto& o=l3_locked();if(!o.enabled)return;
        o.begin_submission();uint64_t draws=0;char text[diagnostics::text_capacity]{};
        // FPS investigation, perf_lean (diagnostics only): no per-list l3-link line, the latest-slot l3-counts/l3-repeats at most once per
        // second. The L3 state updates above/below are unchanged.
        auto* lean=diagnostics::sink();const bool leanLog=lean&&lean->perf_lean();
        if(lists)for(UINT i=0;i<count;++i) {
            auto it=s.states.find(listKey_locked(lists[i]));if(it==s.states.end())continue;
            auto* l=l3_list_locked(it->second);if(!l)continue;
            draws=diagnostics::L3Observation::add(draws,o.execute(*l));
            if(!l->draws||leanLog)continue;
            std::snprintf(text,sizeof text,"l3-link;queue=%llx;sub=%llx;ordinal=%x;list=%llx;gen=%llx;draws=%llx;Reset=%llx;Close=%llx;closed=%s;executions=%llx;gap=%u;completion=unknown",
                static_cast<uint64_t>(reinterpret_cast<uintptr_t>(self)),o.submission,i,it->second.bindings.list_identity,l->generation,
                l->draws,l->resets,l->closes,l->closes?(l->closed?"yes":"failed"):"unknown",l->executes,unsigned(l->gap));
            diagnostics::emit(diagnostics::record(diagnostics::Event::requirements_metadata,0,0,0,text));
        }
        o.end_submission(draws);
        if(leanLog) {
            static uint64_t s_lastCounts=0;const uint64_t now=GetTickCount64();
            if(s_lastCounts&&now-s_lastCounts<1000)return;
            s_lastCounts=now;
        }
        o.counts(text,sizeof text);
        diagnostics::emit(diagnostics::record(diagnostics::Event::requirements_metadata,0,0,0,text));
        o.repeat_text(text,sizeof text);
        diagnostics::emit(diagnostics::record(diagnostics::Event::requirements_metadata,0,0,0,text));
    });
    observeSafely([&]{copyBeforeExecute(self,count,lists);});
    Step1Submit step1Submit;Step5Submit step5Submit;
    const int phase=g_step1Phase.load(std::memory_order_relaxed);
    if(phase==static_cast<int>(Step1Phase::inserted))
        observeSafely([&]{step1BeforeExecute(self,count,lists,step1Submit);});
    else if(phase==static_cast<int>(Step1Phase::continuous))observeSafely([&]{step5BeforeExecute(self,count,lists,step5Submit);});
    // DECOUPLE review 3 (Astra open risk 2): fail closed at the Execute boundary. Outside the continuous phase no owned-recording re-execution is
    // examined, so every frame submitted before this Execute stops being provably unchanged (no diagnostic callback involved).
    if(phase!=static_cast<int>(Step1Phase::continuous))step5ExecuteUnexamined();
    const uint64_t perfT1=perf::ticks();
    B3Exec b3x;brief3dBeforeExecute(self,count,lists,b3x); // BRIEF3D live: batches with replay lists take the submit mutex (off: one relaxed load)
    fn(self,count,lists); // Preserve count, array and ordering exactly.
    brief3dAfterExecute(self,b3x); // BRIEF3D live: one Signal of the producer fence on this queue
    const uint64_t perfT2=perf::ticks();
    // Private Signal after the native batch returned; no hook lock is held.
    if(step1Submit.ticket||step1Submit.witnessed)step1AfterExecute(step1Submit);
    if(step5Submit.signal)step5AfterExecute(step5Submit); // J-3b8: one Signal per published frame
    diagnostics::flush_after_execute(); // All observation/callback guards unwound.
    perf::add(perf::execute,(perfT1-perfT0)+(perf::ticks()-perfT2));
}
bool isWrappedIID(REFIID iid) {
    return iid==__uuidof(IUnknown) || iid==__uuidof(IDXGIObject) ||
      iid==__uuidof(IDXGIFactory) || iid==__uuidof(IDXGIFactory1) ||
      iid==__uuidof(IDXGIFactory2) || iid==__uuidof(IDXGIFactory3) ||
      iid==__uuidof(IDXGIFactory4) || iid==__uuidof(IDXGIFactory5) || iid==__uuidof(IDXGIFactory6);
}
// VSCREEN (Astra review 6455891169 finding 3): the swap chain -> queue association for the capture at Present. Only a DIRECT
// ID3D12CommandQueue passed as pDevice of a SUCCESSFUL creation is recorded, with its device; the queue reference taken here is kept for the
// process (the capture may use the queue at any later Present). Recording only: no hook, no other side effect.
void noteVscreenSwapchainQueue(IUnknown* dev,void* swapchain) {
#ifdef ACZ_VR_OFFLINE_TEST
    (void)dev;(void)swapchain;   // the fake-COM fixtures of this file never see the VSCREEN association (no extra reference)
#else
    if(!dev||!swapchain) return;
    ID3D12CommandQueue* queue=nullptr;
    if(FAILED(dev->QueryInterface(IID_PPV_ARGS(&queue)))||!queue) return;
    std::unique_ptr<ID3D12CommandQueue,ReleaseCOM> ownedQueue(queue);
    if(queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT) return;
    ID3D12Device* device=nullptr;
    if(FAILED(queue->GetDevice(IID_PPV_ARGS(&device)))||!device) return;
    std::unique_ptr<ID3D12Device,ReleaseCOM> ownedDevice(device);   // the queue keeps its device alive; only the identity is recorded
    acz::vscreen::note_swapchain_queue(swapchain,ownedQueue.release(),device);
#endif
}
void captureDeviceAndQueue(IUnknown* dev) {
    HookAdmission admission;if(!admission)return;
    if(!dev) return;
    ID3D12CommandQueue* queue=nullptr;
    if(FAILED(dev->QueryInterface(IID_PPV_ARGS(&queue)))) return;
    std::unique_ptr<ID3D12CommandQueue,ReleaseCOM> ownedQueue(queue);
    const auto queueDesc=queue->GetDesc();
    if(queueDesc.Type!=D3D12_COMMAND_LIST_TYPE_DIRECT) return;
    ID3D12Device* device=nullptr;
    if(FAILED(queue->GetDevice(IID_PPV_ARGS(&device)))) return;
    std::unique_ptr<ID3D12Device,ReleaseCOM> ownedDevice(device);
    diagnostics::emit(diagnostics::record(diagnostics::Event::device,1,0,0,"device and direct queue discovered"));
    // Step-1 owner (default OFF): authenticate the first device/queue by the
    // references retained below, BEFORE device hooks can witness list or
    // allocator creations against them.
    if(step1Enabled()) {
        bool first=false;{auto& s=state();std::lock_guard lock(s.mutex);first=!s.device;}
        if(first)step1WitnessDeviceQueue(device,queue,queueDesc);
    } else step1ReportMissingOptIn();
#ifndef ACZ_VR_OFFLINE_TEST
    // Future base-device creations are observed; pre-existing objects fail closed.
    hookNativeBindingDevice(device);
#endif
    if(!hookCommandQueue(queue)) {
        OutputDebugStringA("[ACZ-VR] command-queue hook incomplete\n");
        return;
    }
    DeviceReadyCallback cb;
    bool accepted=false;
    {
        auto& s=state(); std::lock_guard lock(s.mutex);
        if(!s.device) {
            cb=s.deviceCb;
            s.device=ownedDevice.release(); s.queue=ownedQueue.release();
            accepted=true;
        }
    }
    if(accepted) {
        diagnostics::emit(diagnostics::record(diagnostics::Event::device,1,1));
        notify(cb,device,queue);
        step1Start(); // no-op unless the step-1 owner exists
    }
}
} // namespace

void setDeviceReadyCallback(DeviceReadyCallback cb) {HookAdmission admission;if(!admission)return;auto& s=state(); std::lock_guard lock(s.mutex); s.deviceCb=std::move(cb);}
void setDrawCallback(DrawCallback cb) {HookAdmission admission;if(!admission)return;auto& s=state(); std::lock_guard lock(s.mutex); s.drawCb=std::move(cb);}
void setExecuteCallback(ExecuteCallback cb) {HookAdmission admission;if(!admission)return;auto& s=state(); std::lock_guard lock(s.mutex); s.executeCb=std::move(cb);}
void setBindingCallback(BindingCallback cb) {HookAdmission admission;if(!admission)return;auto& s=state(); std::lock_guard lock(s.mutex); s.bindingCb=std::move(cb);}
// Borrowed references, retained for process lifetime by HookState.
ID3D12Device* getHookedDevice() {auto& s=state(); std::lock_guard lock(s.mutex); return s.device;}
ID3D12CommandQueue* getHookedQueue() {auto& s=state(); std::lock_guard lock(s.mutex); return s.queue;}
// J-xr-stereo-output: the step-1 owner's saved-native-dispatch rule, exported.
void* nativeDispatch(void* object,int slot) noexcept {
    if(!object||slot<0||slot>=static_cast<int>(std::tuple_size<decltype(Hooks::originals)>::value))return nullptr;
    try {return step1Native(object,slot);} catch(...) {return nullptr;}
}
namespace {std::atomic<StopHook> g_orderlyStop{nullptr};}
void setOrderlyStopHook(StopHook hook) noexcept {g_orderlyStop.store(hook,std::memory_order_release);}
namespace {
thread_local unsigned g_proxyScope=0;
std::atomic<unsigned> g_xrWindow{0};
// Astra issue 29 (P2): the window is a bounded, counted exception, never
// provenance. It opens when the first window call enters and stays usable for
// at most kXrWindowCapMs from that entry, however long the call blocks.
std::atomic<uint64_t> g_xrWindowStart{0};
std::atomic<uint64_t> g_xrWindowTolerated{0},g_xrWindowExpired{0};
}
void enterProxyCreationScope(bool window) noexcept {
    ++g_proxyScope;
    if(window&&g_xrWindow.fetch_add(1,std::memory_order_acq_rel)==0)g_xrWindowStart.store(GetTickCount64(),std::memory_order_release);
}
void leaveProxyCreationScope(bool window) noexcept {
    if(g_proxyScope)--g_proxyScope;
    if(window){unsigned v=g_xrWindow.load();while(v&&!g_xrWindow.compare_exchange_weak(v,v-1)){}}
}
bool proxyCreationScopeActive() noexcept {return g_proxyScope!=0;}
bool xrCallWindowOpen() noexcept {return g_xrWindow.load(std::memory_order_acquire)!=0;}
bool xrCallWindowActive() noexcept {
    if(!xrCallWindowOpen())return false;
    return GetTickCount64()-g_xrWindowStart.load(std::memory_order_acquire)<=kXrWindowCapMs; // a stuck call never extends it
}
uint64_t xrWindowTolerated() noexcept {return g_xrWindowTolerated.load();}
uint64_t xrWindowExpired() noexcept {return g_xrWindowExpired.load();}
bool loadRealDXGI() {
    auto& loading=g_loading;
    if(loading) return false; // Loader reentry must not wait on its own once_flag.
    struct Guard { bool& flag; ~Guard(){flag=false;} } guard{loading};
    loading=true;
#ifdef ACZ_VR_OFFLINE_TEST
    auto& once=*g_loadOnceTest;
#else
    auto& once=g_loadOnce;
#endif
    std::call_once(once, [] {
        // A loader-body failure is terminal, including allocation exceptions.
        // Catch INSIDE call_once so later calls do not silently retry loading.
        try {
#ifdef ACZ_VR_OFFLINE_TEST
        g_realDXGI=g_loadTest?g_loadTest():nullptr;
#else
        wchar_t directory[MAX_PATH]{};
        UINT length=GetSystemDirectoryW(directory,MAX_PATH);
        if(!length || length>=MAX_PATH) return;
        std::wstring path(directory,length); path+=L"\\dxgi.dll";
        g_realDXGI=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
#endif
        } catch(...) {g_realDXGI=nullptr;}
    });
    return g_realDXGI!=nullptr;
}
namespace {
bool listTableAvailable_locked(void** table) {
    const auto& s=state();
    return !s.compatibility.count(table)&&!s.devices.count(table)&&!s.objects.count(table)&&!s.queues.count(table);
}
bool installListTable_locked(void** table) {
    auto& s=state();if(!listTableAvailable_locked(table))return false;auto& hooks=s.lists[table];
    // Lifetime/reset prerequisites precede draws, including partial recovery.
    return patchRelease_locked(table,reinterpret_cast<void*>(&hookedRelease),hooks) &&
        patch(table,VTIDX_Reset,reinterpret_cast<void*>(&hookedReset),hooks) &&
        patch(table,VTIDX_ClearState,reinterpret_cast<void*>(&hookedClearState),hooks) &&
        patch(table,VTIDX_SetPipelineState,reinterpret_cast<void*>(&hookedSetPipelineState),hooks) &&
        patch(table,VTIDX_DrawInstanced,reinterpret_cast<void*>(&hookedDrawInstanced),hooks) &&
        patch(table,VTIDX_DrawIndexedInstanced,reinterpret_cast<void*>(&hookedDrawIndexedInstanced),hooks);
}
}
// Called only for explicit/orderly teardown. Never waits for an active hook.
// false means CPU cleanup is incomplete (GPU veto, busy, or allocation failure).
// Experimental admission may already be closed. Retry only from an owner-
// controlled quiescent context outside loader lock. Module pin remains.
bool shutdownDiagnosticOwnership(bool processTerminating) noexcept {
    if(processTerminating)return true; // Never dispatch foreign cleanup at detach.
    acz::feature_log::flush(); // J-3b9c: dedicated evidence logs (orderly path only)
    // J-xr-stereo-output: ask the XR owner to stop (atomic store, no wait). Its
    // own thread retires its swapchains/session; it keeps its device/queue refs.
    if(auto hook=g_orderlyStop.load(std::memory_order_acquire))hook();
    // The step-1 worker may still use the device/queue; never drain under it.
    if(!step1Quiescent())return false;
    // Close GPU admission atomically against reservations BEFORE the CPU drain.
    // This does not close hook admission; stopping an experiment preserves Draw
    // and Execute forwarding. Pending/quarantined owners veto all dependencies.
    if(!gpu_copy::diagnostic_session().begin_global_drain())return false;
    if(!ownershipWasEnabled.load())return true;
    static std::atomic<bool> cleanupComplete{false};
    if(cleanupComplete.load())return true;
    if(ownershipShutdown.stopped())return false; // closed is not a cleanup join
    struct Drain {
        std::array<OwnershipPin,16> pins{};
        std::array<ID3D12Resource*,SrvCandidates::capacity> candidates{};
        std::array<ID3D12Resource*,diagnostics::UploadCreationPins<ID3D12Resource>::capacity> uploads{};
        IUnknown* device=nullptr;IUnknown* queue=nullptr;
        decltype(HookState::states) lists;
        decltype(HookState::pipelines) pipelines;
    };
    // Large storage is heap-only; allocation failure leaves ownership intact.
    std::unique_ptr<Drain> drain(new(std::nothrow) Drain);
    if(!drain)return false;
    if(!ownershipShutdown.close())return cleanupComplete.load();
    auto* s=allocatedState.load();if(!s){cleanupComplete.store(true);return true;}
    {std::lock_guard lock(s->mutex);
     for(size_t i=0;i<s->ownershipPins.size();++i)std::swap(s->ownershipPins[i],drain->pins[i]);
     for(size_t i=0;i<SrvCandidates::capacity;++i) {
         drain->candidates[i]=s->srvCandidates.entries[i].held;
         s->srvCandidates.entries[i]={};
     }
     for(auto& heap:s->srvCandidates.heaps)heap={};
     s->srvCandidates.held=0;
     for(size_t i=0;i<drain->uploads.size();++i)drain->uploads[i]=s->uploadPins.take(i);
     s->states.swap(drain->lists);s->pipelines.swap(drain->pipelines);
     s->pipelineIndex.clear();++s->pipelineEpoch; // FPS growth fix: rebuilt (empty) by the next lookup
     drain->device=s->device;s->device=nullptr;drain->queue=s->queue;s->queue=nullptr;
    }
    // All storage is empty before the first Release. Reentrant hooks refuse;
    // shared roots are released once by their control block, never per copy.
    for(auto& pin:drain->pins) {
        for(size_t i=0;i<pin.interfaces.size();++i)if(pin.counts[i])releaseOwned(pin.interfaces[i]);
        if(pin.rootCanonicalOwned)releaseOwned(pin.rootCanonical);
        if(pin.texture)releaseOwned(pin.texture);
        if(pin.buffer&&pin.counts[0])releaseOwned(pin.buffer);
        pin.root.reset();
    }
    for(auto* candidate:drain->candidates)releaseOwned(candidate);
    for(auto* upload:drain->uploads)releaseOwned(upload);
    drain->lists.clear();drain->pipelines.clear();
    releaseOwned(drain->queue);releaseOwned(drain->device);
    cleanupComplete.store(true);
    return true;
}

bool hookCommandList(ID3D12GraphicsCommandList* list) {
    HookAdmission admission;if(!admission)return false;
    if(!list || !pinProxy()) return false;
    auto& s=state(); std::lock_guard lock(s.mutex);
    void** table=vtable(list);if(!listTableAvailable_locked(table))return false;
    // Preserve a state entry even when prerequisites are only partly installed.
    if(!patchRelease_locked(table,reinterpret_cast<void*>(&hookedRelease),s.lists[table]))return false;
    s.states.try_emplace(listKey_locked(list));s.states[listKey_locked(list)].table=table;
    return installListTable_locked(table);
}
bool hookCommandQueue(ID3D12CommandQueue* queue) {
    HookAdmission admission;if(!admission)return false;
    if(!queue || !pinProxy()) return false;
    auto& s=state(); std::lock_guard lock(s.mutex);
    void** table=vtable(queue);
    if(s.compatibility.count(table))return false;
    return patch(table,VTIDX_ExecuteCommandLists,reinterpret_cast<void*>(&hookedExecuteCommandLists),s.queues[table]);
}
void markPSOHasGS(ID3D12PipelineState* pso,bool gs) {HookAdmission admission;if(!admission)return;auto& s=state(); std::lock_guard lock(s.mutex); s.gs[pso]=gs;}
void forgetPSO(ID3D12PipelineState* pso) {HookAdmission admission;if(!admission)return;auto& s=state(); std::lock_guard lock(s.mutex); s.gs.erase(pso);}

namespace {
template<class T> std::shared_ptr<T> retain(T* p,ULONG* count=nullptr) {
    if(!p) return {};
    const auto refs=p->AddRef();if(count)*count=refs;
    return {p,[](T* value){if(ownershipShutdown.stopped())releaseOwned(value);else value->Release();}};
}
// Called only by the successful matched creation path while ALL QI refs live.
// Reserve before COM, never recycle, never decrement a PSO pin (even on failure).
unsigned pinCreatedPipeline(ID3D12PipelineState* base,capture::Id identity,
    IUnknown* canonical,const std::array<IUnknown*,6>& ancestors,
    const std::shared_ptr<ID3D12RootSignature>& root,ULONG rootCount,unsigned& refusals) {
    auto* log=diagnostics::sink();
    if(!log||!log->ownership_pins()){refusals|=PinRefusals::switch_off;return 0;}
    if(!rootCount){refusals|=PinRefusals::reference;return 0;}
    auto& s=state();unsigned slot=0;
    {std::lock_guard lock(s.mutex);
     auto key=s.rootKeys.find(root.get());
     if(key==s.rootKeys.end()){refusals|=PinRefusals::provenance;return 0;}
     auto creation=s.rootCreations.find(key->second);auto token=s.rootIdentities.find(key->second);
     const bool accepted=creation!=s.rootCreations.end()&&token!=s.rootIdentities.end()&&
         token->second&&creation->second.identity==token->second;
     const bool sameInterface=creation!=s.rootCreations.end()&&creation->second.interfaceAddress==root.get();
     if(!accepted)refusals|=PinRefusals::provenance;
     if(creation!=s.rootCreations.end()&&!sameInterface)refusals|=PinRefusals::creation_interface;
     if(!accepted||!sameInterface)return 0;
     // Only a complete pin on this retained base can be shared. Never retry a
     // partial/faulted acquisition or assume a newly returned tear-off is held.
     for(size_t i=0;i<s.ownershipPins.size();++i) {
         const auto& held=s.ownershipPins[i];
         if(!held.reserved||held.base!=base)continue;
         bool matches=held.complete&&held.root.get()==root.get()&&held.rootKey==key->second&&
             held.rootIdentity==token->second;
         auto owns=[&](IUnknown* address) {
             return address&&std::find(held.interfaces.begin(),held.interfaces.end(),address)!=held.interfaces.end();
         };
         matches=matches&&owns(base)&&owns(canonical);
         for(auto* address:ancestors)if(address)matches=matches&&owns(address);
         if(!matches){refusals|=PinRefusals::reference;return 0;}
         if(s.pinDedupeHits<65535)++s.pinDedupeHits;
         return static_cast<unsigned>(i+1);
     }
     for(;slot<s.ownershipPins.size();++slot)if(!s.ownershipPins[slot].reserved)break;
     if(slot==s.ownershipPins.size()) {
         if(s.pinsCapRefused<65535)++s.pinsCapRefused;refusals|=PinRefusals::cap;return 0;
     }
     auto& pin=s.ownershipPins[slot];pin.reserved=true;pin.base=base;pin.identity=identity;
     pin.root=root;pin.rootCount=rootCount;pin.rootKey=key->second;pin.rootIdentity=token->second;
     std::array<IUnknown*,8> addresses{};addresses[0]=base;addresses[1]=canonical;
     std::copy(ancestors.begin(),ancestors.end(),addresses.begin()+2);
     size_t count=0;
     for(auto* p:addresses)if(p&&std::find(pin.interfaces.begin(),pin.interfaces.end(),p)==pin.interfaces.end())
         pin.interfaces[count++]=p;
    }
    // Own interface dispatch, no mutex, no arbitrary-object probe. A nonpositive
    // diagnostic return or exception consumes the slot but never grants proof.
    auto& pin=s.ownershipPins[slot];bool complete=true;
    if(pin.rootKey!=root.get()) {
        // Exactly one QI through the positively retained BASE, never through a
        // historical canonical address. The returned QI AddRef is itself the
        // canonical own-reference pin. No cleanup/retry even on bad HRESULT,
        // null, mismatch or an exception with unknown reference completion.
        {std::lock_guard lock(s.mutex);
         if(s.pinRefusals.qiAttempts<65535)++s.pinRefusals.qiAttempts;}
        IUnknown* acquired=nullptr;HRESULT hr=E_FAIL;bool returned=false;
        try {hr=root->QueryInterface(__uuidof(IUnknown),reinterpret_cast<void**>(&acquired));returned=true;}
        catch(...) {} // Own no usable certificate; never speculate with Release.
        {std::lock_guard lock(s.mutex);
         pin.rootCanonical=acquired;
         pin.rootCanonicalOwned=returned&&SUCCEEDED(hr)&&acquired;
         auto key=s.rootKeys.find(root.get());auto token=s.rootIdentities.find(pin.rootKey);
         auto creation=s.rootCreations.find(pin.rootKey);
         complete=returned&&SUCCEEDED(hr)&&acquired&&
             acquired==reinterpret_cast<IUnknown*>(pin.rootKey)&&
             key!=s.rootKeys.end()&&key->second==pin.rootKey&&
             token!=s.rootIdentities.end()&&token->second==pin.rootIdentity&&
             creation!=s.rootCreations.end()&&creation->second.identity==pin.rootIdentity&&
             creation->second.interfaceAddress==root.get();
         if(complete&&s.pinRefusals.qiSuccesses<65535)++s.pinRefusals.qiSuccesses;}
        if(!complete){refusals|=PinRefusals::split_root|PinRefusals::root_qi;return 0;}
    }
    // If AddRef throws, observeSafely catches it but the summary retains why.
    refusals|=PinRefusals::reference;
    for(size_t i=0;i<pin.interfaces.size();++i)if(auto* p=pin.interfaces[i]) {
        const ULONG refs=p->AddRef();
        {std::lock_guard lock(s.mutex);pin.counts[i]=refs;}
        complete=refs!=0&&complete;
    }
    {std::lock_guard lock(s.mutex);pin.complete=complete;
     if(complete&&s.pinsTaken<65535)++s.pinsTaken;}
    if(complete)refusals&=~PinRefusals::reference;
    return complete?slot+1:0;
}
bool pinnedPipeline_locked(const PipelineRecord& p,ID3D12PipelineState* base) noexcept {
    if(!p.ownershipPin||p.ownershipPin>state().ownershipPins.size())return false;
    const auto& pin=state().ownershipPins[p.ownershipPin-1];
    return pin.complete&&pin.rootCount&&pin.base==base&&pin.identity==p.ownershipIdentity&&
        (pin.rootKey==pin.root.get()||pin.rootCanonical==reinterpret_cast<IUnknown*>(pin.rootKey))&&
        pin.root.get()==p.root.get()&&pin.rootIdentity==p.root_identity;
}
bool pinnedRoot_locked(const PipelineRecord& p,ID3D12RootSignature* bound) noexcept {
    if(!p.ownershipPin||p.ownershipPin>state().ownershipPins.size())return false;
    const auto& pin=state().ownershipPins[p.ownershipPin-1];
    return pin.complete&&pin.rootCount&&pin.rootIdentity==p.root_identity&&
        pin.root.get()==p.root.get()&&bound&&
        (bound==pin.root.get()||reinterpret_cast<IUnknown*>(bound)==pin.rootCanonical);
}
// Deliberately isolated SEH leaf: no C++ object with a destructor, allocation,
// native method or unwinding through the guard. Only memcpy is fault guarded.
__declspec(noinline) bool guardedCpuRead(const void* source,void* dest,size_t bytes) noexcept {
    __try {std::memcpy(dest,source,bytes);}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION||GetExceptionCode()==EXCEPTION_IN_PAGE_ERROR?
        EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return false;}
    return true;
}
#ifdef ACZ_VR_OFFLINE_TEST
static diagnostics::CpuUploadSnapshot::Reader g_cpuReader=&guardedCpuRead;
#endif
HRESULT STDMETHODCALLTYPE l3Map(ID3D12Resource*,UINT,const D3D12_RANGE*,void**);
void STDMETHODCALLTYPE l3Unmap(ID3D12Resource*,UINT,const D3D12_RANGE*);
// Same-address supported QI aliases share this table. Distinct/unsupported
// aliases disable nativeBindings in queryBindingInterface. Do not read a vptr
// through a historical resource: the table was captured at live creation.
bool cpuMapCoverage_locked(const diagnostics::L3Observation::Resource* r) noexcept {
    if(!r||!r->hooks||!r->map_table)return false;
    const auto it=state().objects.find(r->map_table);
    return it!=state().objects.end()&&it->second.originals[8]&&it->second.originals[9]&&
        !it->second.pendingProtection[8]&&!it->second.pendingProtection[9]&&
        r->map_table[8]==reinterpret_cast<void*>(&l3Map)&&
        r->map_table[9]==reinterpret_cast<void*>(&l3Unmap);
}
bool cpuRequestCurrent_locked(void* self,const diagnostics::CpuUploadSnapshot::Request& q) noexcept {
    auto& s=state();auto* log=diagnostics::sink();
    if(!log||!log->ownership_pins()||!s.nativeBindings.enabled()||s.bindingEvents.load()||s.rootCreates||s.metadataReleases||
       bindingCohort().clock!=q.observation_epoch||bindingCohort().clock==UINT64_MAX)return false;
    const auto it=s.states.find(listKey_locked(self));if(it==s.states.end())return false;
    const auto& b=it->second.bindings;
    if(b.list_identity!=q.list||b.generation!=q.generation||b.revision!=q.revision||!b.tracking_complete)return false;
    const auto* selected=l3_list_locked(it->second);
    if(!selected||selected->draws!=q.draw||b.topology!=q.topology||b.pipeline.strip_cut!=q.strip_cut)return false;
    const auto p=s.pipelines.find(it->second.pso.get());
    if(p==s.pipelines.end()||!p->second.drawcpt_identity_matched||listPointFailure_locked(it->second,b)||
       pipelineIdentityFailure_locked(p->second,b)||!pinnedPipeline_locked(p->second,it->second.pso.get())||
       !pinnedRoot_locked(p->second,it->second.root.get()))return false;
    for(const auto& x:q.ranges) {
        if(s.nativeBindings.resource_token(x.resource)!=binding::Token{x.resource,x.lifetime})return false;
        const auto* r=s.l3.resources.get(x.resource);
        if(!r||!x.pinned||s.uploadPins.lookup(x.resource,r->serial,x.lifetime,x.map_generation,x.mapped))return false;
        if(!cpuMapCoverage_locked(r)||r->retired||r->map_generation!=x.map_generation||r->mapped!=x.mapped||
           r->desc.Width!=x.resource_bytes||r->map_inflight||(x.mapped&&
           (r->mapped_lifetime!=x.lifetime||r->map_gap||r->outstanding!=1||r->mapped_subresource!=0)))return false;
    }
    return true;
}
__declspec(noinline) void emitCpuSnapshot_locked(const diagnostics::CpuUploadSnapshot::Request& q,bool queued) noexcept {
    auto& out=state().cpuSnapshot;
    constexpr const char* kinds[]={"cpu-t10","cpu-vb","cpu-ib"};
    char text[diagnostics::text_capacity]{};
    for(size_t i=0;i<3;++i) {
        const auto& x=q.ranges[i];
        std::snprintf(text,sizeof text,"%s;sampled-diagnostic;list=%llx;gen=%llx;draw=%llx;resource=%llx;life=%llx;map-gen=%llx;va=%llx;offset=%llx;bytes=%llx;hash=%016llx;pin=%u;mapped=%u;stable=%u;first=%s",
            kinds[i],q.list,q.generation,q.draw,x.resource,x.lifetime,x.map_generation,x.va,x.offset,x.bytes,out.hashes[i],unsigned(x.pinned),unsigned(x.mapped!=nullptr),unsigned(out.stable),out.reason);
        diagnostics::emit(diagnostics::record(diagnostics::Event::requirements_metadata,0,0,0,text));
    }
    std::snprintf(text,sizeof text,"cpu-snapshot;sampled-diagnostic;list=%llx;gen=%llx;draw=%llx;stable=%u;first=%s;index=%s;min=%llx;max=%llx;vertices=%llx;stride=%x;index-stride=%x;start=%x;count=%x;base=%d;raw=%u;attempt=%zu;no-coherence;no-gpu-completion;capture=disabled",
        q.list,q.generation,q.draw,unsigned(out.stable),out.reason,out.index_reason,out.minimum,out.maximum,out.vertices,q.vertex_stride,q.index_stride,q.start,q.count,q.base,unsigned(queued),out.attempts);
    diagnostics::emit(diagnostics::record(diagnostics::Event::requirements_metadata,0,0,0,text));
}
void finishCpuSnapshot(void* self,diagnostics::CpuUploadSnapshot::Request& q) noexcept {
    auto& s=state();
    // Serialize final validation, both bounded sample passes and postvalidation
    // with observed Map/Unmap invalidation. Application writes through mapped
    // pointers bypass these hooks: equal samples prove no coherence or GPU completion.
    std::lock_guard lock(s.mutex);
    auto* log=diagnostics::sink();if(!log||!log->ownership_pins())return;
    // Pure bounded lookups (at most 3 x 32 entries). Never dispatch COM through
    // historical Draw metadata; creation-time ownership survives last game Release.
    const char* preflight=nullptr;
    for(auto& x:q.ranges) {
        const auto* r=s.l3.resources.get(x.resource);
        const char* reason=s.uploadPins.lookup(x.resource,r?r->serial:0,x.lifetime,x.map_generation,x.mapped);
        x.pinned=!reason;
        if(!reason&&!cpuMapCoverage_locked(r))reason="map-unmap-coverage-incomplete";
        if(reason&&!preflight) {
            preflight=r&&std::string_view(reason)=="upload-pin-not-created"?
                r->upload_pin_reason:reason;
        }
    }
    if(!preflight&&!cpuRequestCurrent_locked(self,q))preflight="stale-request";
    if(!preflight)for(const auto& x:q.ranges)if(!x.va){preflight="unknown-gpu-va";break;}
    auto& out=s.cpuSnapshot;bool ok=false;
    if(preflight) {
        out.file_bytes=0;out.stable=false;out.hashes.fill(0);out.minimum=out.maximum=out.vertices=0;
        out.reason=preflight;out.index_reason="not-evaluated";
    } else {
#ifdef ACZ_VR_OFFLINE_TEST
        ok=out.read(q,g_cpuReader);
#else
        ok=out.read(q,&guardedCpuRead);
#endif
        if(ok)out.sampled_ms=GetTickCount64(); // J-3b6 comparison timestamp only
    }
    if(ok&&(!out.matches(q)||!cpuRequestCurrent_locked(self,q))) {
        ok=false;out.stable=false;out.file_bytes=0;out.hashes.fill(0);
        out.reason="stale-request-after-sample";out.index_reason="not-evaluated";
    }
    if(ok) {
        const capture::Command cmd{true,q.count,q.instances,q.start,q.base,q.start_instance};
        observeRequirements_locked(s.states.at(listKey_locked(self)),cmd,q.descriptor_before,nullptr,&q,self);
    }
    const bool queued=ok&&log->raw_snapshot(out.file.data(),out.file_bytes);
    emitCpuSnapshot_locked(q,queued);
}

template<class F> F creationOriginal(void* self,int slot,bool device) {
    auto& s=state();std::lock_guard lock(s.mutex);
    auto& m=device?s.devices:s.objects;auto it=m.find(vtable(self));
    return it==m.end()?nullptr:reinterpret_cast<F>(it->second.originals[slot]);
}
HRESULT STDMETHODCALLTYPE l3Map(ID3D12Resource* self,UINT sub,const D3D12_RANGE* read,void** data) {
    HookAdmission admission;if(!admission)return E_FAIL;
    using Fn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Resource*,UINT,const D3D12_RANGE*,void**);
    uint64_t serial=0;binding::Token token{};
    {auto& s=state();std::lock_guard lock(s.mutex);if(l3_locked().enabled)
        if(auto* r=s.l3.resources.get(reinterpret_cast<uintptr_t>(self))) {
            serial=r->serial;token=s.nativeBindings.resource_token(reinterpret_cast<uintptr_t>(self));r->invalidate_map();if(r->map_inflight<UINT32_MAX)++r->map_inflight;else r->map_gap=true;
        }}
    auto hr=creationOriginal<Fn>(self,8,false)(self,sub,read,data);
    auto& s=state();std::lock_guard lock(s.mutex);auto& o=l3_locked();
    auto* r=o.resources.get(reinterpret_cast<uintptr_t>(self));
    if(r&&r->serial==serial) {
        if(r->map_inflight)--r->map_inflight;
        const bool current=token.id&&s.nativeBindings.resource_token(reinterpret_cast<uintptr_t>(self))==token;
        o.map(reinterpret_cast<uintptr_t>(self),SUCCEEDED(hr),SUCCEEDED(hr)&&data&&*data,sub,current&&SUCCEEDED(hr)&&data?*data:nullptr,token.generation);
    } else if(!r)o.map(reinterpret_cast<uintptr_t>(self),SUCCEEDED(hr),SUCCEEDED(hr)&&data&&*data,sub);
    if(r)s.uploadPins.map(r->key,r->serial,r->mapped_lifetime,r->map_generation,r->mapped);
    return hr;
}
void STDMETHODCALLTYPE l3Unmap(ID3D12Resource* self,UINT sub,const D3D12_RANGE* written) {
    HookAdmission admission;if(!admission)return;
    using Fn=void(STDMETHODCALLTYPE*)(ID3D12Resource*,UINT,const D3D12_RANGE*);
    {auto& s=state();std::lock_guard lock(s.mutex);l3_locked().unmap(reinterpret_cast<uintptr_t>(self),sub);}
    creationOriginal<Fn>(self,9,false)(self,sub,written);
}
HRESULT STDMETHODCALLTYPE l3Write(ID3D12Resource* self,UINT sub,const D3D12_BOX* box,const void* data,UINT row,UINT depth) {
    HookAdmission admission;if(!admission)return E_FAIL;
    using Fn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Resource*,UINT,const D3D12_BOX*,const void*,UINT,UINT);
    auto hr=creationOriginal<Fn>(self,12,false)(self,sub,box,data,row,depth);
    if(SUCCEEDED(hr)){auto& s=state();std::lock_guard lock(s.mutex);l3_locked().cpu_write(reinterpret_cast<uintptr_t>(self));}return hr;
}
void l3Created(HRESULT hr,void** out,REFIID iid,const D3D12_RESOURCE_DESC* desc,unsigned kind,
               const D3D12_HEAP_PROPERTIES* props=nullptr,bool record_creation=true) noexcept {
    if(FAILED(hr)||!out||!*out)return;
    try {
        auto& s=state();std::lock_guard lock(s.mutex);
        // J-155 fix: a new object at the watched world-RT address (textures included, before the
        // buffer-only L3 gate) is a new incarnation; earlier facts no longer match it.
        if(record_creation) {
            // Never let an unsupported/reused creation inherit an older table proof.
            s.step5WorldLifetimes.erase(reinterpret_cast<uintptr_t>(*out));
            // A newly returned texture is live; establish Release coverage before publishing
            // its incarnation. No COM ownership, QI or historical-pointer dispatch.
            if(desc&&desc->Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&
               (desc->Flags&D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)&&iid==__uuidof(ID3D12Resource))
                step5TrackWorldLifetime_locked(static_cast<IUnknown*>(*out),true);
            step5NoteWatchLifetime_locked(reinterpret_cast<uintptr_t>(*out),true);
        }
        auto& o=l3_locked();if(!o.enabled)return;
        const auto address=reinterpret_cast<uintptr_t>(*out);if(record_creation)o.create(address,desc,kind,props);
        // Exact output IID only. No QI, GetHeapProperties or ownership.
        if(iid!=__uuidof(ID3D12Resource)){
            if(!record_creation)diagnostics::L3Observation::inc(o.patch_gaps);
            return;
        }
        auto* table=vtable(*out);
        if(s.devices.count(table)||s.lists.count(table)||s.queues.count(table)||s.compatibility.count(table)) {
            diagnostics::L3Observation::inc(o.patch_gaps);return;
        }
        // No allocation in this L3 map-hook path. The world-texture path above
        // may establish a new Release dispatch from the live creation return.
        auto owner=s.objects.find(table);if(owner==s.objects.end())return;
        auto& hooks=owner->second;
        if(record_creation) {
            // A creation return alone must not install Map/Unmap/Write seams.
            // Reuse established resource seams, or install those only after the
            // unchanged base-buffer adapter's existing validation/lifetime step.
            const bool covered=hooks.originals[8]&&hooks.originals[9]&&hooks.originals[12]&&
                table[8]==reinterpret_cast<void*>(&l3Map)&&table[9]==reinterpret_cast<void*>(&l3Unmap)&&table[12]==reinterpret_cast<void*>(&l3Write);
            if(auto* r=o.resources.get(address)){r->hooks=covered;r->map_table=table;}
            return;
        }
        bool ok=patch(table,8,reinterpret_cast<void*>(&l3Map),hooks);
        ok=patch(table,9,reinterpret_cast<void*>(&l3Unmap),hooks)&&ok;
        ok=patch(table,12,reinterpret_cast<void*>(&l3Write),hooks)&&ok;
        if(auto* r=o.resources.get(address)){r->hooks=ok;r->map_table=table;}
        if(!ok)diagnostics::L3Observation::inc(o.patch_gaps);
    }catch(...) {auto& s=state();std::lock_guard lock(s.mutex);diagnostics::L3Observation::inc(s.l3.patch_gaps);}
}
// Called ONLY before a successful native creation returns its output to the
// caller. The output is live here. Reserve under mutex, AddRef outside, publish
// under mutex. No GetHeapProperties/QI or historical-pointer acquisition.
void pinUploadCreation(HRESULT hr,void** out,REFIID iid,const D3D12_RESOURCE_DESC* desc,
                       const D3D12_HEAP_PROPERTIES* props,bool published=false,const BindingRecord* event=nullptr) noexcept {
    auto* log=diagnostics::sink();if(!log||!log->ownership_pins())return;
    if(hr!=S_OK||!out||!*out)return; // S_FALSE is not a validated creation
    auto& s=state();using Pins=diagnostics::UploadCreationPins<ID3D12Resource>;
    size_t slot=Pins::capacity;binding::Token token{};
    const auto address=reinterpret_cast<uintptr_t>(*out);
    {std::lock_guard lock(s.mutex);
     const char* reason=nullptr;
     if(!desc)reason="upload-pin-description-unavailable";
     else if(desc->Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER)reason="upload-pin-not-buffer";
     else if(!props)reason="upload-pin-heap-unavailable"; // placed/reserved lack known heap properties
     else if(props->Type!=D3D12_HEAP_TYPE_UPLOAD)reason="upload-pin-not-upload";
     else if(!desc->Width||desc->Width>Pins::per_buffer_cap)reason="upload-pin-oversize";
     else if(iid!=__uuidof(ID3D12Resource))reason="upload-pin-iid-unsupported";
     auto* r=s.l3.resources.get(address);
     if(!reason&&(!r||!r->serial))reason="upload-pin-generation-unavailable";
     token=s.nativeBindings.resource_token(address);
     if(!reason&&published&&(!event||event->tainted||s.bindingEvents.load()!=1||!s.nativeBindings.enabled()||!token.id||s.nativeBindings.untracked(address)))
         reason="upload-pin-publication-unavailable";
     if(!reason)slot=s.uploadPins.reserve(address,r->serial,desc->Width);
     else s.uploadPins.last_reason=reason;
     if(r)r->upload_pin_reason=s.uploadPins.last_reason;
    }
    if(slot==Pins::capacity)return;
    auto* resource=static_cast<ID3D12Resource*>(*out);ULONG refs=0;
    try {refs=resource->AddRef();}catch(...) {} // unknown completion: consume slot, no retry
    bool stale=false;
    {std::lock_guard lock(s.mutex);
     const auto* r=s.l3.resources.get(address);
     stale=!r||r->serial!=s.uploadPins.entries[slot].creation||
         (published&&(!event||event->tainted||s.bindingEvents.load()!=1||!s.nativeBindings.enabled()||s.nativeBindings.resource_token(address)!=token||s.nativeBindings.untracked(address)));
     s.uploadPins.finish(slot,!stale&&refs?resource:nullptr);
     if(stale)s.uploadPins.last_reason="upload-pin-publication-changed";
     if(auto* current=s.l3.resources.get(address))if(current->serial==s.uploadPins.entries[slot].creation)
         current->upload_pin_reason=s.uploadPins.last_reason;
    }
    // Callback invalidated publication: balance only a known acquisition, outside mutex.
    if(stale&&refs)try {resource->Release();}catch(...) {}
}
void bindUploadCreation_locked(uint64_t address) noexcept {
    auto& s=state();if(const auto* r=s.l3.resources.get(address))
        s.uploadPins.bind(address,r->serial,s.nativeBindings.resource_token(address).generation);
}
ULONG STDMETHODCALLTYPE releaseMetadata(IUnknown* self) {
    HookAdmission admission;if(!admission)return 0;
    perf::Scope cost(perf::release); // FPS growth investigation (observe only; includes the native Release and the hook-mutex waits)
    using Fn=ULONG(STDMETHODCALLTYPE*)(IUnknown*);
    Fn fn=nullptr;ID3D12GraphicsCommandList* listKey=nullptr;
    capture::Id listToken=0;bool listKnown=false,metadata=false;uint64_t l3serial=0,candidateHeapSerial=0,worldSerial=0,watchSerial=0;
    {auto& s=state();std::lock_guard lock(s.mutex);
     auto* hooks=releaseHooks_locked(vtable(self),false);
     if(hooks)fn=reinterpret_cast<Fn>(hooks->originals[2]);
     listKey=listKey_locked(self);auto list=s.states.find(listKey);
     listKnown=list!=s.states.end();if(listKnown)listToken=list->second.bindings.list_identity;
     auto m=s.objects.find(vtable(self));metadata=m!=s.objects.end()&&m->second.metadataRelease;
     if(auto r=s.step5WorldLifetimes.find(reinterpret_cast<uintptr_t>(self));r!=s.step5WorldLifetimes.end())worldSerial=r->second.serial;
     if(g_step5WorldWatch.load()==reinterpret_cast<uintptr_t>(self))watchSerial=s.step5WatchSerial;
     if(l3_locked().enabled)if(auto* r=s.l3.resources.get(reinterpret_cast<uintptr_t>(self)))l3serial=r->serial;}
    // Installed entries always retain their native dispatch, including partial
    // protection recovery. Never call another hook as an original.
    if(!fn)std::terminate();
    ID3D12RootSignature* rootKey=reinterpret_cast<ID3D12RootSignature*>(self);
    {auto& s=state();std::lock_guard lock(s.mutex);
     auto k=s.rootKeys.find(self);if(k!=s.rootKeys.end())rootKey=k->second;}
    MetadataFootprint before;
    capture::Id beforePublication=0;
    binding::Token heap,resource;
    capture::Id pipelineIdentity=0, rootIdentity=0;
    ID3D12PipelineState* pipelineKey=nullptr;
    {auto& s=state();std::lock_guard lock(s.mutex);
     before.objects[0]=reinterpret_cast<uintptr_t>(rootKey);
     before.objects[1]=reinterpret_cast<uintptr_t>(self);
     beforePublication=s.rootEventSerial;
     heap=s.nativeBindings.heap_token(reinterpret_cast<uintptr_t>(self));
     if(heap.id)if(const auto* d=s.nativeBindings.diagnostic()) {
         const auto h=d->heaps.find(heap.id);
         if(h!=d->heaps.end())candidateHeapSerial=h->second.serial;
     }
     resource=s.nativeBindings.resource_token(reinterpret_cast<uintptr_t>(self));
     auto p=pipelineFor_locked(self);
     if(p!=s.pipelines.end()) {
         before.releaseComplete=p->second.retirementKnown&&
             std::find(p->second.retirementKeys.begin(),p->second.retirementKeys.end(),
                 reinterpret_cast<uintptr_t>(self))!=p->second.retirementKeys.end();
         if(!p->second.creation_interface_supported||
            p->second.canonical==PipelineRecord::Canonical::unavailable||
            std::find(p->second.retirementKeys.begin(),p->second.retirementKeys.end(),
                reinterpret_cast<uintptr_t>(self))==p->second.retirementKeys.end())
             before.releaseRefusals|=unsupported_identity;
         pipelineKey=p->first;pipelineIdentity=p->second.identity;
         before.objects[4]=reinterpret_cast<uintptr_t>(pipelineKey);
         before.objects[5]=reinterpret_cast<uintptr_t>(p->second.canonical_address);
         for(size_t i=0;i<p->second.ancestor_addresses.size();++i)
             before.objects[6+i]=reinterpret_cast<uintptr_t>(p->second.ancestor_addresses[i]);
         auto dependency=p->second.root.get();before.objects[2]=reinterpret_cast<uintptr_t>(dependency);
         auto key=s.rootKeys.find(dependency);
         if(key!=s.rootKeys.end())before.objects[3]=reinterpret_cast<uintptr_t>(key->second);
         for(auto key:p->second.retirementKeys)if(key) {
             auto slot=std::find(before.objects.begin(),before.objects.end(),key);
             if(slot==before.objects.end()) {
                 slot=std::find(before.objects.begin(),before.objects.end(),uint64_t(0));
                 if(slot==before.objects.end()) {before.releaseComplete=false;before.overflow=true;}
                 else *slot=key;
             }
         }
     }
     auto r=s.rootIdentities.find(rootKey);
     if(r!=s.rootIdentities.end()) rootIdentity=r->second;
     auto rc=s.rootCreations.find(rootKey);
     if(!pipelineKey&&rc!=s.rootCreations.end()) {
         before.releaseComplete=rc->second.retirementKnown&&
             std::find(rc->second.retirementKeys.begin(),rc->second.retirementKeys.end(),
                 reinterpret_cast<uintptr_t>(self))!=rc->second.retirementKeys.end();
         if(std::find(rc->second.retirementKeys.begin(),rc->second.retirementKeys.end(),
                reinterpret_cast<uintptr_t>(self))==rc->second.retirementKeys.end())
             before.releaseRefusals|=unsupported_identity;
         for(auto key:rc->second.retirementKeys)if(key) {
             auto slot=std::find(before.objects.begin(),before.objects.end(),key);
             if(slot==before.objects.end()) {
                 slot=std::find(before.objects.begin(),before.objects.end(),uint64_t(0));
                 if(slot==before.objects.end()) {before.releaseComplete=false;before.overflow=true;}
                 else *slot=key;
             }
         }
     }
     if(!pipelineKey&&rc==s.rootCreations.end()) {
         before.releaseRefusals|=unregistered;
         before.creationSighting=s.creationSightings.classify(reinterpret_cast<uintptr_t>(self));
         before.sightingUnavailable=before.creationSighting==2?
             (self?CreationSightings::capacity_lost:CreationSightings::other):0u;
         before.sightingClock=bindingCohort().clock;
     }
     // Shared-table unregistered objects, unsupported identities and binding
     // owners have no complete metadata-only retirement certificate.
     const auto contains=[&](IUnknown* key) {
         return std::find(before.objects.begin(),before.objects.end(),reinterpret_cast<uintptr_t>(key))!=before.objects.end();
     };
     before.releaseComplete=before.releaseComplete&&contains(self)&&!heap.id&&!resource.id;
     if(!contains(self))before.releaseRefusals|=unsupported_identity;
     if(heap.id||resource.id||listKnown)before.releaseRefusals|=binding_owner;
     for(auto key:before.objects)if(key) {
         if(s.nativeBindings.heap_token(key).id||s.nativeBindings.resource_token(key).id||
            s.listKeys.count(reinterpret_cast<IUnknown*>(key))||
            s.states.count(reinterpret_cast<ID3D12GraphicsCommandList*>(key))) {
             before.releaseComplete=false;before.releaseRefusals|=binding_owner;
         }
     }
     auto* pinLog=diagnostics::sink();
     before.ownershipCandidate=pinLog&&pinLog->ownership_pins()&&
         (before.releaseRefusals==unregistered)&&!before.overflow;
     if(before.ownershipCandidate)for(auto key:before.objects)
         if(key&&registeredOwnershipKey_locked(key))before.ownershipCandidate=false;
    }
    // Keep the token from BEFORE the original Release. A new object at the
    // same address must not be erased by cleanup for this retired lifetime.
    // Shared vtables also serve untracked objects. Only a tracked binding
    // lifetime can conflict with binding publication; unrelated COM releases
    // must not join the cohort or enlarge its reentry/unknown-footprint surface.
    const auto refs=fn(self);
    if(!refs&&candidateHeapSerial)retireSrvCandidates(candidateHeapSerial,reinterpret_cast<uintptr_t>(self));
    // Compare PRE-call identities: native Release can reenter a creation wrapper
    // and publish a new incarnation at the same address before returning zero.
    if(!refs&&worldSerial) {
        auto& s=state();std::lock_guard lock(s.mutex);
        auto r=s.step5WorldLifetimes.find(reinterpret_cast<uintptr_t>(self));
        if(r!=s.step5WorldLifetimes.end()&&r->second.serial==worldSerial) {
            s.step5WorldLifetimes.erase(r);
            if(g_step5WorldWatch.load()==reinterpret_cast<uintptr_t>(self)&&s.step5WatchSerial==watchSerial)
                step5NoteWatchLifetime_locked(reinterpret_cast<uintptr_t>(self),false);
        }
    }
    if(!refs&&l3serial) {auto& s=state();std::lock_guard lock(s.mutex);
        if(auto* r=s.l3.resources.get(reinterpret_cast<uintptr_t>(self));r&&r->serial==l3serial) {
            r->invalidate_map();r->retired=true;r->version=s.l3.tick();
        }}
    if(!refs&&listKnown) {
        (void)capture::new_lifetime_identity(); // former empty retirement ListState
        ListStates::node_type old;auto& s=state();
        {std::lock_guard lock(s.mutex);auto it=s.states.find(listKey);
         if(it!=s.states.end()&&it->second.bindings.list_identity==listToken) {
             if(it->second.copyObservation)it->second.copyObservation->uncertain("list-released");
             step1ListReleased(listKey); // registry leaf mutex; no-op while OFF
             old=s.states.extract(it);
             for(auto alias=s.listKeys.begin();alias!=s.listKeys.end();)
                 if(alias->second==listKey)alias=s.listKeys.erase(alias);else ++alias;
         }} // Owned COM cleanup occurs after unlocking, never under the mutex.
        // J-180 P1: the destroyed list's recording can never execute again (hook-state mutex dropped; lock order step-1 then hook state).
        step5NoteListReleased(step1Route(listKey));
    }
    // Registered list owners are disjoint from metadata owners. Unrelated
    // metadata peers sharing this table still use the original metadata path.
    if(!metadata||listKnown)return refs;
    if(!refs) {
        // Like device Release, a non-final metadata Release publishes nothing.
        // Keep no open observation across native COM calls. Snapshot only opaque
        // keys/tokens above; never dereference self after native destruction.
        {auto& s=state();std::lock_guard lock(s.mutex);
         // Metadata activity during native destruction is not described by a
         // pre-call certificate. Leave it refused rather than infer safety.
         if(s.rootEventSerial!=beforePublication) {
             before.releaseComplete=false;before.releaseRefusals|=native_activity;
         }}
        MetadataEvent retirement(reinterpret_cast<IUnknown*>(rootKey),false);
        {auto& s=state();std::lock_guard lock(s.mutex);
         for(auto key:before.objects)retirement.touch(reinterpret_cast<IUnknown*>(key));
         if(before.overflow) {
             retirement.footprint.overflow=true;retirement.footprint.known=false;
             retirement.mark(MetadataEvent::capacity);
         }
         retirement.footprint.ownershipCandidate=before.ownershipCandidate;
         retirement.footprint.releaseComplete=before.releaseComplete&&!retirement.footprint.overflow;
         retirement.footprint.releaseRefusals=before.releaseRefusals;
         retirement.footprint.creationSighting=before.creationSighting;
         retirement.footprint.sightingUnavailable=before.sightingUnavailable;
         retirement.footprint.sightingClock=before.sightingClock;
         // A root published during this call at a retiring address/dependency
         // cannot retain provenance after returned-zero, even if its token is
         // newer. Plain token replacement without creation is still protected.
         for(auto it=s.rootCreations.begin();it!=s.rootCreations.end();) {
             bool conflict=false;
             for(auto key:before.objects)if(key)conflict|=
                 key==reinterpret_cast<uintptr_t>(it->first) ||
                 key==reinterpret_cast<uintptr_t>(it->second.interfaceAddress);
             if(conflict && it->second.publication>beforePublication) {
                 diagnostics::emit(diagnostics::record(diagnostics::Event::root,0,0,0,
                     "root rejected: final_release_conflict; parent=metadata_release"));
                 const auto key=reinterpret_cast<uintptr_t>(it->first);
                 for(auto& list:s.states)if(list.second.bindings.root_id==key) {
                     list.second.bindings.root_identity=0;++list.second.bindings.revision;
                 }
                 s.rootIdentities.erase(it->first);s.roots.erase(it->first);
                 it=s.rootCreations.erase(it);
             } else ++it;
         }}
        std::optional<BindingEvent> event;
        if(heap.id || resource.id)event.emplace(reinterpret_cast<uintptr_t>(self),true,
            heap.id?binding::ObservationKind::heap_release:binding::ObservationKind::resource_release);
        observeNativeBinding([&](auto& b){
            // A completed creation inside native final Release may have
            // replaced the pre-call token before retirement joins the cohort.
            // Token-checked removal alone would preserve that ambiguous lifetime.
            // Compare and invalidate atomically under the observation lock; never
            // touch COM here. In-flight creations still meet the retirement event.
            const auto key=reinterpret_cast<uintptr_t>(self);
            const auto currentHeap=b.heap_token(key),currentResource=b.resource_token(key);
            if((heap.id || resource.id) &&
               ((currentHeap.id && currentHeap!=heap) ||
                (currentResource.id && currentResource!=resource)))
                b.invalidate_object(key);
            b.remove_heap(heap);b.remove_resource(resource);
        });
        PipelineRecord old;
        auto& s=state();
        {std::lock_guard lock(s.mutex);
         auto p=pipelineKey;
         auto it=s.pipelines.find(p);
         if(it!=s.pipelines.end() && it->second.identity==pipelineIdentity) {
             old=std::move(it->second);s.pipelines.erase(it);s.gs.erase(p);
             pipelineIndexEdit_locked(reinterpret_cast<uintptr_t>(p),&old,nullptr); // FPS growth fix
         }
         auto r=rootKey;
         auto ri=s.rootIdentities.find(r);
         if(ri!=s.rootIdentities.end() && ri->second==rootIdentity) {
             for(auto& list:s.states)if(list.second.bindings.root_id==reinterpret_cast<uintptr_t>(r) &&
                 list.second.bindings.root_identity==rootIdentity) {
                 list.second.bindings.root_identity=0;++list.second.bindings.revision;
             }
             s.rootIdentities.erase(ri);s.roots.erase(r);s.rootCreations.erase(r);
         }
         // Rejected provenance can still have registered Release aliases.
         // Preserve aliases only if a newer live token replaced this lifetime.
         ri=s.rootIdentities.find(r);
         if(ri==s.rootIdentities.end()) {
             for(auto k=s.rootKeys.begin();k!=s.rootKeys.end();)
                 if(k->second==r)k=s.rootKeys.erase(k);else ++k;
         }
        }
    }
    // Keep retirement marked busy through token-compared cleanup; otherwise a
    // creation could mistake the returned-zero, not-yet-erased lifetime as live.
    return refs;
}
bool trackMetadataLifetime(IUnknown* obj) {
    std::lock_guard lock(state().mutex);return trackMetadataLifetime_locked(obj);
}
// Validate PSO identity at creation only. Third-address ancestors additionally
// require checked Release coverage and unique routing before publication.
// All QI refs survive publication; no COM work is added at Draw.
struct PipelineInterfaces {
    std::array<std::unique_ptr<IUnknown,ReleaseCOM>,15> refs;
    IUnknown* canonical=nullptr;
    std::array<IUnknown*,6> ancestors{};
    uint32_t ancestorCovered=0; // creation snapshot, never refreshed at Draw
    bool valid=false;
    diagnostics::LifetimeConditions conditions;
    // Slots: Object base/canonical, DeviceChild base/canonical, Pageable base/canonical.
    // Two queries per slot: requested interface, then its canonical IUnknown.
    uint32_t qiFailed=0,identityMismatch=0,nullOutput=0,alias=0,hrClasses=0;
    void format(char* text,size_t size,const diagnostics::LifetimeConditions& order) const {
        std::snprintf(text,size,";qf=%03x;qm=%02x;qn=%03x;qa=%02x;qh=%06x;od=%04x/%04x;ac=%02x",
            qiFailed,identityMismatch,nullOutput,alias,hrClasses,order.known,order.value,ancestorCovered);
    }
    void observe(ID3D12PipelineState* raw) {
        auto query=[&](IUnknown* source,REFIID iid,size_t index) {
            IUnknown* value=nullptr;
            const auto hr=source->QueryInterface(iid,reinterpret_cast<void**>(&value));
            refs[index].reset(value);
            return SUCCEEDED(hr)?value:nullptr;
        };
        auto* base=reinterpret_cast<IUnknown*>(raw);
        canonical=query(base,__uuidof(IUnknown),0);
        if(!canonical)return;
        conditions.set(diagnostics::split_address,canonical!=base);
        // Preserve the existing self-canonical contract. Additional seams
        // below are required only for the newly supported split shape.
        if(canonical==base) {valid=true;return;}
        const bool stable=query(canonical,__uuidof(IUnknown),1)==canonical;
        conditions.set(diagnostics::stable_qi,stable);
        const bool roundTrip=stable&&query(canonical,__uuidof(ID3D12PipelineState),2)==base;
        if(stable)conditions.set(diagnostics::round_trip,roundTrip);
        const IID iids[]={__uuidof(ID3D12Object),__uuidof(ID3D12DeviceChild),__uuidof(ID3D12Pageable)};
        bool eligible=stable&&roundTrip;
        for(size_t i=0;i<std::size(iids);++i)for(size_t side=0;side<2;++side) {
            const auto slot=2*i+side,index=3+4*i+2*side;
            auto diagnosticQuery=[&](IUnknown* source,REFIID iid,size_t ref,unsigned stage) {
                IUnknown* value=nullptr;
                const auto hr=source->QueryInterface(iid,reinterpret_cast<void**>(&value));
                refs[ref].reset(value);
                const auto q=unsigned(2*slot+stage);
                hrClasses|=(SUCCEEDED(hr)?1u:hr==E_NOINTERFACE?2u:3u)<<(2*q);
                if(FAILED(hr))qiFailed|=1u<<q;
                if(!value)nullOutput|=1u<<q;
                return SUCCEEDED(hr)?value:nullptr;
            };
            auto* value=diagnosticQuery(side?canonical:base,iids[i],index,0);
            const bool primaryAddress=value&&(value==canonical||value==base);
            if(value&&!primaryAddress)alias|=1u<<slot;
            // Identity alone is insufficient: the creation wrapper must cover
            // this output's Release and publish its per-object routing.
            auto* identity=value?diagnosticQuery(value,__uuidof(IUnknown),index+1,1):nullptr;
            if(identity&&identity!=canonical)identityMismatch|=1u<<slot;
            const bool identityMatched=value&&identity==canonical;
            if(identityMatched)ancestors[slot]=value;
            // Preserve the original diagnostic masks' short-circuit semantics.
            if(eligible)conditions.set(uint32_t(diagnostics::object_base)<<slot,identityMatched);
            eligible=eligible&&identityMatched;
        }
        valid=eligible;
    }
};
HRESULT STDMETHODCALLTYPE queryBindingInterface(IUnknown* self,REFIID iid,void** out) {
    HookAdmission admission;if(!admission)return E_FAIL;
    using Fn=HRESULT(STDMETHODCALLTYPE*)(IUnknown*,REFIID,void**);
    auto hr=creationOriginal<Fn>(self,0,false)(self,iid,out);
    if(SUCCEEDED(hr) && out && *out) observeNativeBinding([&](auto& b){
        const auto object=reinterpret_cast<uintptr_t>(self);
        const bool resource=b.resource_token(object).id!=0,heap=b.heap_token(object).id!=0;
        const bool common=iid==__uuidof(IUnknown) || iid==__uuidof(ID3D12Object) ||
                          iid==__uuidof(ID3D12DeviceChild) || iid==__uuidof(ID3D12Pageable);
        if((resource || heap) && (*out!=self || !(common ||
           (resource && iid==__uuidof(ID3D12Resource)) || (heap && iid==__uuidof(ID3D12DescriptorHeap))))) b.disable("binding interface alias unsupported");
    });
    return hr;
}
bool trackBindingLifetime(IUnknown* obj) {
    if(!trackMetadataLifetime(obj)) return false;
    auto& s=state();std::lock_guard lock(s.mutex);auto table=vtable(obj);
    return patch(table,0,reinterpret_cast<void*>(&queryBindingInterface),s.objects[table]);
}
// Continuity still requires J026's globally isolated event ordering. Fresh
// provenance has a separate, strictly object-specific publication gate.
struct RootCreationEvent : MetadataEvent {
    RootCreationEvent():MetadataEvent(nullptr,true) {}
    bool current(const HookState& s) const {
        return isolated && serial && s.rootEventSerial==serial && s.rootCreates==1 && !s.metadataReleases &&
            !s.bindingEvents.load() && bindingCohort().clock==footprint.begin;
    }
};
// Failure/conflict invalidates live provenance without changing historical copies.
// No COM owner is removed here. Bound values become unusable with identity zero.
struct RootCreationInvalidation {
    ID3D12RootSignature* address;
    capture::Id publication;
    bool preserved=false;
    ~RootCreationInvalidation() {
        if(preserved) return;
        auto& s=state();std::lock_guard lock(s.mutex);
        auto record=s.rootCreations.find(address);
        // An older/failed observation never rolls back a newer publication.
        if(record!=s.rootCreations.end() && record->second.publication!=publication) return;
        auto id=s.rootIdentities.find(address);
        for(auto& list:s.states) if(list.second.bindings.root_id==reinterpret_cast<uintptr_t>(address) &&
            (id==s.rootIdentities.end() || list.second.bindings.root_identity!=id->second)) {
            list.second.bindings.root_identity=0;++list.second.bindings.revision;
        }
    }
};
#ifdef ACZ_VR_OFFLINE_TEST
bool g_rootMetadataAllocation=true;
unsigned g_rootPublishFailure=0;
// Deterministic CPU-only publication scheduling; never present in the DLL.
thread_local std::function<void()> g_rootReady;
#endif
HRESULT STDMETHODCALLTYPE createRoot(ID3D12Device* self,UINT node,const void* bytes,SIZE_T length,REFIID iid,void** out) {
    HookAdmission admission;if(!admission)return E_FAIL;
    using Fn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,UINT,const void*,SIZE_T,REFIID,void**);
    // Owners precede the event so even exceptional cleanup releases AFTER the
    // event ends, outside the mutex. Keep every QI reference through publication.
    std::unique_ptr<ID3D12RootSignature,ReleaseCOM> owned;
    std::unique_ptr<IUnknown,ReleaseCOM> canonical,baseCanonical,canonicalSelf;
    std::unique_ptr<ID3D12RootSignature,ReleaseCOM> roundTrip;
    RootCreationEvent event;
    struct RootDiagnostic {
        bool accepted=false;
        // Stable reason strings identify the precise gate; b is the literal
        // SerializedRootFailure enum, c the native/QI/SDK HRESULT (uint32 bits).
        // Context is bounded, allocation-free and emitted through the sink only.
        const char* reason="root rejected: native_failure";
        uint32_t failure=0, result=0;
        UINT node; SIZE_T length; IID iid;
        unsigned order=0,creates=0,releases=0,bindings=0;
        const char* blocking="none";
        const char* overlap="";
        std::array<char,64> detail{};
        // Per-record equivalence-class tags: 0 absent, 1 base, 2/3 distinct.
        // No address bits or process-global allocation/registration required.
        const void *base=nullptr,*root=nullptr,*canonical=nullptr;
        const char* form="not_checked";
        diagnostics::LifetimeConditions conditions;
        unsigned tag(const void* p) const {return !p?0:p==base?1:p==root?2:3;}
        ~RootDiagnostic(){
            char text[diagnostics::text_capacity]{};
            std::snprintf(text,sizeof text,"%s; %s node=%u bytes=%zu iid=%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x order=%u creates=%u releases=%u bindings=%u ids=%u/%u/%u eq=%u block=%s form=%s detail=%s",
                reason,overlap,node,length,iid.Data1,iid.Data2,iid.Data3,iid.Data4[0],iid.Data4[1],iid.Data4[2],iid.Data4[3],iid.Data4[4],iid.Data4[5],iid.Data4[6],iid.Data4[7],order,creates,releases,bindings,tag(base),tag(root),tag(canonical),
                unsigned(base&&base==root)|(unsigned(base&&base==canonical)<<1)|(unsigned(root&&root==canonical)<<2),blocking,form,detail.data());
            const auto used=std::strlen(text);
            std::snprintf(text+used,sizeof(text)-used,";ld=%06x/%06x",conditions.known,conditions.value);
            diagnostics::emit(diagnostics::record(diagnostics::Event::root,accepted,failure,result,text));
        }
    } diagnostic{false,"root rejected: native_failure",0,0,node,length,iid};
    auto hr=creationOriginal<Fn>(self,16,true)(self,node,bytes,length,iid,out);
    observeCreationReturn(hr,out,CreationSightings::root);
    if(FAILED(hr)) {auto& s=state();std::lock_guard lock(s.mutex);event.footprint.known=true;}
    diagnostic.result=static_cast<uint32_t>(hr);
    if(SUCCEEDED(hr)) diagnostic.reason=!out?"root rejected: null_out":"root rejected: null_object";
    if(SUCCEEDED(hr) && out && *out) observeSafely([&] {
      try {
        diagnostic.reason="root rejected: observation_exception";
        RootCreationRecord previous;
        capture::Id previousIdentity=0;
        bool hadProvenance=false;
        auto invalidate=[&](ID3D12RootSignature* address,bool candidate) {
            auto& s=state();std::lock_guard lock(s.mutex);
            auto id=s.rootIdentities.find(address);
            auto old=s.rootCreations.find(address);
            if(candidate) hadProvenance=id!=s.rootIdentities.end() || old!=s.rootCreations.end() || s.roots.count(address);
            if(event.serial && old!=s.rootCreations.end() && old->second.publication>event.serial) return false;
            if(candidate && id!=s.rootIdentities.end() && old!=s.rootCreations.end() &&
               old->second.identity==id->second && s.roots.count(address)) {
                previousIdentity=id->second;previous=std::move(old->second);
            }
            s.rootCreations.erase(address);s.rootIdentities.erase(address);s.roots.erase(address);
            return true;
        };
        diagnostic.base=*out;
        auto address=static_cast<ID3D12RootSignature*>(*out);
        {auto& s=state();std::lock_guard lock(s.mutex);
         auto k=s.rootKeys.find(static_cast<IUnknown*>(*out));if(k!=s.rootKeys.end())address=k->second;}
        {auto& s=state();std::lock_guard lock(s.mutex);
         event.identify(reinterpret_cast<IUnknown*>(address));
         // The returned address alone does not identify its canonical aliases.
         event.footprint.known=false;}
        RootCreationInvalidation invalidation{address,event.serial};
        if(!invalidate(address,true)) {diagnostic.reason="root rejected: stale_invalidation";return;}
        if(iid!=__uuidof(ID3D12RootSignature) && iid!=__uuidof(IUnknown)) {diagnostic.reason="root rejected: unsupported_iid";return;}
        ID3D12RootSignature* raw=nullptr;
        auto qi=static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&raw));
        owned.reset(raw);diagnostic.root=raw;
        diagnostic.result=static_cast<uint32_t>(qi);
        if(FAILED(qi)) {diagnostic.reason="root rejected: root_qi_failed";return;}
        if(!raw) {diagnostic.reason="root rejected: root_qi_null";return;}
        IUnknown* identity=nullptr;qi=raw->QueryInterface(IID_PPV_ARGS(&identity));
        canonical.reset(identity);diagnostic.canonical=identity;
        diagnostic.result=static_cast<uint32_t>(qi);
        if(FAILED(qi) || !identity) {diagnostic.reason="root rejected: canonical_qi_failed";return;}
        diagnostic.conditions.set(diagnostics::split_address,identity!=reinterpret_cast<IUnknown*>(raw));
        IUnknown* baseIdentity=nullptr;
        qi=static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&baseIdentity));
        baseCanonical.reset(baseIdentity);diagnostic.result=static_cast<uint32_t>(qi);
        if(FAILED(qi) || !baseIdentity) {diagnostic.reason="root rejected: canonical_qi_failed";return;}
        if(baseIdentity!=identity) {
            RootCreationInvalidation aliasInvalidation{raw,event.serial};
            invalidate(raw,false);diagnostic.reason="root rejected: root_alias";return;
        }
        auto key=reinterpret_cast<ID3D12RootSignature*>(identity);
        if(*out!=raw && *out!=identity) {
            RootCreationInvalidation aliasInvalidation{key,event.serial};
            invalidate(key,false);diagnostic.reason="root rejected: root_alias";return;
        }
        diagnostic.conditions.set(diagnostics::supported_output,true);
        IUnknown* stable=nullptr;
        qi=identity->QueryInterface(IID_PPV_ARGS(&stable));canonicalSelf.reset(stable);
        diagnostic.result=static_cast<uint32_t>(qi);
        diagnostic.conditions.set(diagnostics::stable_qi,SUCCEEDED(qi)&&stable==identity);
        if(FAILED(qi) || stable!=identity) {
            RootCreationInvalidation aliasInvalidation{key,event.serial};
            invalidate(key,false);diagnostic.reason=FAILED(qi)?"root rejected: canonical_qi_failed":"root rejected: canonical_mismatch";return;
        }
        // Validate the identity's root interface as well: malformed aliases
        // must not merge records merely by claiming another object's IUnknown.
        ID3D12RootSignature* check=nullptr;
        qi=identity->QueryInterface(IID_PPV_ARGS(&check));roundTrip.reset(check);
        diagnostic.result=static_cast<uint32_t>(qi);
        diagnostic.conditions.set(diagnostics::round_trip,SUCCEEDED(qi)&&check==raw);
        if(FAILED(qi) || check!=raw) {
            RootCreationInvalidation aliasInvalidation{key,event.serial};
            invalidate(key,false);diagnostic.reason=FAILED(qi)?"root rejected: root_qi_failed":"root rejected: canonical_mismatch";return;
        }
        if(key!=address) {
            invalidation.address=key;
            if(!invalidate(key,true)) {diagnostic.reason="root rejected: stale_invalidation";return;}
        }
        {auto& s=state();std::lock_guard lock(s.mutex);
         event.touch(raw);
         event.identify(identity);}
        if(previousIdentity && previous.interfaceAddress!=raw) {
            diagnostic.reason="root rejected: root_alias";return;
        }
        {auto& s=state();std::lock_guard lock(s.mutex);
         for(const auto& alias:s.rootKeys)
             if(alias.second==key && alias.first!=identity && alias.first!=raw) {
                 diagnostic.reason="root rejected: root_alias";return;
             }
         auto alias=s.rootKeys.find(raw);
         if(alias!=s.rootKeys.end() && alias->second!=key) {
             diagnostic.reason="root rejected: root_alias";return;
         }}
#ifdef ACZ_VR_OFFLINE_TEST
        if(g_rootReady) {auto ready=std::move(g_rootReady);g_rootReady={};ready();}
#endif
        if(!bytes) {diagnostic.reason="root rejected: null_blob";return;}
        if(!length) {diagnostic.reason="root rejected: empty_blob";return;}
        if(length>metadata::RootBindingMetadata::max_serialized_bytes) {diagnostic.reason="root rejected: oversized_blob";return;}
#ifdef ACZ_VR_OFFLINE_TEST
        if(!g_rootMetadataAllocation) throw std::bad_alloc();
        if(g_rootPublishFailure==3) throw 3; // Non-allocation observation exception.
#endif
        RootCreationRecord record;
        const auto* first=static_cast<const uint8_t*>(bytes);
        record.bytes.assign(first,first+length);
        record.device=self;record.node=node;record.iid=iid;record.interfaceAddress=raw;
        diagnostic.form=record.bytes.size()>=4 && std::equal(record.bytes.begin(),record.bytes.begin()+4,"DXBC")?
            "dxbc":"other";
        auto decoded=metadata::RootBindingMetadata::from_serialized(record.bytes.data(),record.bytes.size(),
            diagnostics::sink()&&diagnostics::sink()->ownership_pins());
        if(!decoded.valid()) {
            diagnostic.reason="root rejected: serialized_failure";
            diagnostic.failure=static_cast<uint32_t>(decoded.failure);
            std::snprintf(diagnostic.detail.data(),diagnostic.detail.size(),"%s",decoded.detail.c_str());
            diagnostic.result=static_cast<uint32_t>(decoded.sdk_hresult);return;
        }
        {auto& s=state();std::lock_guard lock(s.mutex);
         if(pipelineFor_locked(reinterpret_cast<IUnknown*>(raw))!=s.pipelines.end()||
            pipelineFor_locked(identity)!=s.pipelines.end()) {
             diagnostic.reason="root rejected: root_alias";return;
         }}
        const bool baseInstalled=trackMetadataLifetime(raw);
        diagnostic.conditions.set(diagnostics::base_install,baseInstalled);
        bool canonicalInstalled=false;
        if(baseInstalled) {
            canonicalInstalled=trackMetadataLifetime(identity);
            diagnostic.conditions.set(diagnostics::canonical_install,canonicalInstalled);
        }
        {auto& s=state();std::lock_guard lock(s.mutex);
         releaseConditions_locked(diagnostic.conditions,raw,false);
         releaseConditions_locked(diagnostic.conditions,identity,true);}
        if(!baseInstalled || !canonicalInstalled){diagnostic.reason="root rejected: lifetime_hook";return;}
        bool consistent=previousIdentity && previous.device==self && previous.node==node &&
            previous.iid==iid && previous.bytes==record.bytes;
        auto& s=state();std::lock_guard lock(s.mutex);
        const bool isolated=event.current(s);
        const bool distinct=event.distinct(s);
        diagnostic.conditions.set(diagnostics::distinct_event,distinct);
        diagnostic.blocking=event.blocking;
        diagnostic.overlap=distinct?"":event.overlap;
        // order bits: initially isolated, currently isolated, ambiguous,
        // prior provenance, independent active/completed footprints. A binding
        // count alone never excludes root provenance; its footprint does.
        diagnostic.order=unsigned(event.isolated) | (unsigned(isolated)<<1) |
            (unsigned(event.ambiguous)<<2) | (unsigned(hadProvenance)<<3) |
            (unsigned(distinct)<<4);
        diagnostic.creates=s.rootCreates;diagnostic.releases=s.metadataReleases;
        diagnostic.bindings=s.bindingEvents.load();
        if(!event.serial) {diagnostic.reason="root rejected: identity_exhausted";diagnostic.blocking="event_identity_exhausted";return;}
        // Existing or missing-history provenance is never refreshed under
        // overlap. Only a fresh observation with proven independent history.
        if(!isolated && hadProvenance) {diagnostic.reason="root rejected: continuity_overlap";diagnostic.blocking="continuity_requires_isolation";return;}
        if(!distinct) {
            const auto block=std::string_view(event.blocking);
            diagnostic.reason=(block=="history_lost" || block=="active_metadata_unknown")?
                "root rejected: unresolved_order":"root rejected: ambiguous_order";
            return;
        }
        auto newer=s.rootCreations.find(key);
        if(newer!=s.rootCreations.end() && newer->second.publication>event.serial) {diagnostic.reason="root rejected: stale_publication";return;}
        consistent=consistent && isolated;
        record.identity=consistent?previousIdentity:capture::new_lifetime_identity();
#ifdef ACZ_VR_OFFLINE_TEST
        if(g_rootPublishFailure==4) record.identity=0;
#endif
        record.publication=event.serial;
        record.conditions=diagnostic.conditions;
        record.retirementKeys=event.footprint.objects;
        record.retirementKnown=event.footprint.known&&!event.footprint.overflow;
        if(!record.identity) {diagnostic.reason="root rejected: identity_exhausted";return;}
        try {
            s.rootKeys.insert_or_assign(reinterpret_cast<IUnknown*>(raw),key);
            s.rootKeys.insert_or_assign(identity,key);
            s.rootCreations.insert_or_assign(key,std::move(record));
#ifdef ACZ_VR_OFFLINE_TEST
            if(g_rootPublishFailure==1) throw std::bad_alloc();
#endif
            s.roots.insert_or_assign(key,std::move(*decoded.metadata));
#ifdef ACZ_VR_OFFLINE_TEST
            if(g_rootPublishFailure==2) throw std::bad_alloc();
#endif
            s.rootIdentities.insert_or_assign(key,s.rootCreations.at(key).identity);
            invalidation.preserved=consistent;
            diagnostic.accepted=true;diagnostic.reason="root metadata accepted: explicit canonical provenance";
        } catch(...) {
            // Never leave a partially published token after allocation failure.
            s.rootCreations.erase(key);s.rootIdentities.erase(key);s.roots.erase(key);throw;
        }
      } catch(const std::bad_alloc&) {diagnostic.reason="root rejected: allocation_failure";}
        catch(...) {diagnostic.reason="root rejected: observation_exception";}
    });
    return hr;
}
HRESULT STDMETHODCALLTYPE createGraphicsPSO(ID3D12Device* self,const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,REFIID iid,void** out) {
    HookAdmission admission;if(!admission)return E_FAIL;
    using Fn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,const D3D12_GRAPHICS_PIPELINE_STATE_DESC*,REFIID,void**);
    // Join the same bounded clock/history without any new interception. Keep
    // the dependency even while the native output is still unknown.
    // Temporary QI/displaced-record Releases can reenter metadata tracking.
    // Only nested Release -> PSO decisions can be deferred to the complete
    // creation snapshot; all other reentry still marks both immediately.
    MetadataEvent event(nullptr,false,"pso_create");
    if(desc) {auto& s=state();std::lock_guard lock(s.mutex);
        event.touch(desc->pRootSignature);
        auto key=s.rootKeys.find(desc->pRootSignature);
        if(key!=s.rootKeys.end())event.touch(reinterpret_cast<IUnknown*>(key->second));
    }
    auto hr=creationOriginal<Fn>(self,10,true)(self,desc,iid,out);
    observeCreationReturn(hr,out,CreationSightings::pso);
    // Keep the original PSO summary's numeric fields; append observations only
    // after creation processing, including early refusals. Stage records retain
    // their existing payload. One summary per native call, under the old quota.
    struct PipelineDiagnostic {
        bool success,hasDesc;
        unsigned pinRefusals=0,pin=0;
        diagnostics::LifetimeConditions conditions,dependency;
        std::array<char,208> detail{};
        ~PipelineDiagnostic() {
            char text[diagnostics::text_capacity]{};
            std::snprintf(text,sizeof text,";pd=%06x/%06x;rd=%06x/%06x%s",
                conditions.known,conditions.value,dependency.known,dependency.value,detail.data());
            if(auto* log=diagnostics::sink();log&&log->ownership_pins()) {
                auto& s=state();std::lock_guard lock(s.mutex);const auto n=std::strlen(text);
                std::snprintf(text+n,sizeof(text)-n,";pt=%04x;px=%04x",s.pinsTaken,s.pinsCapRefused);
                // Separate PSO summary has room for counters; requirements
                // context stays byte-for-byte within its existing ceiling.
                if(!pin&&!pinRefusals)pinRefusals=PinRefusals::observation;
                s.pinRefusals.record(pinRefusals);
                s.pinRefusals.append(text,sizeof text,pinRefusals);
                const auto used=std::strlen(text);
                std::snprintf(text+used,sizeof(text)-used,";pu=%04x;ph=%04x;cr=%04x;cw=%04x",
                    s.pinsTaken,s.pinDedupeHits,s.cb10ConstantDraws,s.constantWordCalls);
                s.constantWitnessCounts.append(text,sizeof text);
            }
            diagnostics::emit(diagnostics::record(diagnostics::Event::pso,success,hasDesc,0,text));
        }
    } diagnostic{SUCCEEDED(hr),desc!=nullptr};
    {auto& s=state();std::lock_guard lock(s.mutex);
     event.identify(SUCCEEDED(hr) && out?static_cast<IUnknown*>(*out):nullptr);
     event.footprint.known=false; // includes unidentified QI/canonical aliases
     // A failed native call has no output footprint.
     if(FAILED(hr))event.footprint.known=true;}
    std::array<diagnostics::ShaderIdentity,5> identities{};
    if(desc) {
        const D3D12_SHADER_BYTECODE stages[]={desc->VS,desc->PS,desc->DS,desc->HS,desc->GS};
        for(size_t i=0;i<std::size(stages);++i)if(stages[i].BytecodeLength)
            identities[i]=diagnostics::shader_identity(stages[i].pShaderBytecode,stages[i].BytecodeLength);
        // EFFECTS-OUTSIDE (B): %LOCALAPPDATA%\acz-vr\dump-shaders.txt lists PS prefixes whose pipelines' VS/PS/GS bytecode is saved (read-only copy).
        const auto& dumpTrigger=acz::shaderdump::process_trigger();
        if(dumpTrigger.count&&dumpTrigger.has(acz::shaderdump::hex8(identities[1].hash.data()))) {
            static std::mutex dumpMutex;static unsigned dumpBudget=acz::shaderdump::kFileCap;
            const acz::shaderdump::Stage st[3]={{desc->VS.pShaderBytecode,size_t(desc->VS.BytecodeLength),identities[0].hash.data()},
                {desc->PS.pShaderBytecode,size_t(desc->PS.BytecodeLength),identities[1].hash.data()},
                {desc->GS.pShaderBytecode,size_t(desc->GS.BytecodeLength),identities[4].hash.data()}};
            unsigned wrote=0;
            try {std::lock_guard lock(dumpMutex);wrote=acz::shaderdump::dump(acz::shaderdump::local_path(L"logs\\shaders"),st,3,dumpBudget);} catch(...) {}
            char text[diagnostics::text_capacity]{};
            std::snprintf(text,sizeof text,"shader-dump;ps=%s;vs=%s;gs=%s;files-written=%u;dir=%%LOCALAPPDATA%%\\acz-vr\\logs\\shaders;trigger=dump-shaders.txt",
                identities[1].hash.data(),identities[0].hash.data(),identities[4].hash.data(),wrote);
            diagnostics::emit(diagnostics::record(diagnostics::Event::requirements_metadata,0,0,0,text));
        }
    }
    const bool drawcpt=desc&&!desc->DS.BytecodeLength&&!desc->HS.BytecodeLength&&
        identities[0].identity==diagnostics::Identity::match&&identities[1].identity==diagnostics::Identity::match&&
        identities[4].identity==diagnostics::Identity::match&&
        std::string_view(identities[0].hash.data())=="f4cfd928c11d0a822baeb92e09767134"&&
        std::string_view(identities[1].hash.data())=="e1372ac10d7191c7e4d1c03b66465260"&&
        std::string_view(identities[4].hash.data())=="06084b6262839d7b336b1d8d0d0b86b8";
    if(auto* log=diagnostics::sink();log&&log->ownership_pins()) {
        if(!drawcpt)diagnostic.pinRefusals|=PinRefusals::hash;
        if(FAILED(hr)||!desc||!out||!*out)diagnostic.pinRefusals|=PinRefusals::native_return;
    }
    if(diagnostics::sink()) {
        if(desc) {
            const D3D12_SHADER_BYTECODE stages[]={desc->VS,desc->PS,desc->DS,desc->HS,desc->GS};
            for(size_t i=0;i<std::size(stages);++i)if(stages[i].BytecodeLength) {
                auto event=diagnostics::record(diagnostics::Event::pso,i,stages[i].BytecodeLength);
                event.shader=identities[i];
                diagnostics::emit(event);
            }
        }
    }
    if(SUCCEEDED(hr) && desc && out && *out) observeSafely([&] {
        PipelineRecord displaced;
        auto invalidate=[&](ID3D12PipelineState* address) {
            PipelineRecord old;
            {auto& s=state();std::lock_guard lock(s.mutex);
             auto found=s.pipelines.find(address);
             if(found!=s.pipelines.end()) {old=std::move(found->second);s.pipelines.erase(found);
                 pipelineIndexEdit_locked(reinterpret_cast<uintptr_t>(address),&old,nullptr);} // FPS growth fix
             s.gs.erase(address);}
            // Both COM cleanup and any hooked Release happen outside the lock.
        };
        invalidate(static_cast<ID3D12PipelineState*>(*out));
        ID3D12PipelineState* raw=nullptr;
        if(FAILED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&raw)))||!raw) {
            diagnostic.pinRefusals|=PinRefusals::validation;return;
        }
        std::unique_ptr<ID3D12PipelineState,ReleaseCOM> owned(raw);
        if(raw!=*out) invalidate(raw);
        PipelineRecord record;
        record.strip_cut=static_cast<uint32_t>(desc->IBStripCutValue);
        record.topology_type=static_cast<uint32_t>(desc->PrimitiveTopologyType);
        if(hudConfigured()||worldObserveConfigured()||worldObjectsConfigured()||phaseObserveConfigured()){record.hudVs=identities[0].hash;record.hudPs=identities[1].hash;}
        record.brVs8=acz::shaderdump::hex8(identities[0].hash.data());record.brPs8=acz::shaderdump::hex8(identities[1].hash.data()); // BRIEFING-3D
        record.drawcpt_identity_matched=drawcpt;
        if(worldIdentityConfigured()&&!drawcpt&&!desc->GS.BytecodeLength&&!desc->HS.BytecodeLength&&!desc->DS.BytecodeLength&&
           desc->StreamOutput.NumEntries==0&&desc->VS.BytecodeLength&&desc->PS.BytecodeLength) {
            const auto acm=diagnostics::acm_world_identity(desc->VS.pShaderBytecode,desc->VS.BytecodeLength,
                                                           desc->PS.pShaderBytecode,desc->PS.BytecodeLength);
            // J-180: the PS must match by its decoded hash AND full container SHA-256 (the partial fingerprint is a diagnostic only).
            record.acm_world_matched=acm.vs_match&&acm.ps_match&&desc->InputLayout.NumElements==4&&desc->InputLayout.pInputElementDescs;
            if(acm.ps_unverified&&g_worldPso.psUnverified.load(std::memory_order_relaxed)<4) { // bounded: reveals the live container if it differs from the pin
                char text[diagnostics::text_capacity]{};
                std::snprintf(text,sizeof text,"acm-ps-unverified;fingerprint=match;full-pair=no;vs-match=%u;live-hash=%s;live-sha256=%s;pinned-hash=c1b58903d5b4acf5a46f0a70b5fdb06d;"
                    "action=refused",unsigned(acm.vs_match),acm.ps_hash.data(),acm.ps_digest.data());
                diagnostics::emit(diagnostics::record(diagnostics::Event::requirements_metadata,0,0,0,text));
            }
            if(record.acm_world_matched)for(unsigned i=0;i<4;++i) {
                const auto& e=desc->InputLayout.pInputElementDescs[i];
                record.acm_world_matched&=e.SemanticName&&_stricmp(e.SemanticName,"ATTRIBUTE")==0&&e.SemanticIndex==i&&
                    e.Format==DXGI_FORMAT_R32G32B32A32_FLOAT&&e.InputSlot==0&&e.AlignedByteOffset==16*i&&
                    e.InputSlotClass==D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA&&!e.InstanceDataStepRate;
            }
            worldObjectsPsoSeen(acm.vs_match,acm.vs_hash_only,acm.ps_fingerprint,record.acm_world_matched,acm.ps_match,acm.ps_unverified);
        }
        // J-3b7: render-target/depth/sample facts the step-3 targets must match.
        {auto& o=record.output;o.known=true;o.rt_count=desc->NumRenderTargets;
         for(unsigned i=0;i<8;++i)o.rtv[i]=static_cast<uint32_t>(desc->RTVFormats[i]);
         o.dsv=static_cast<uint32_t>(desc->DSVFormat);o.samples=desc->SampleDesc.Count;o.quality=desc->SampleDesc.Quality;
         o.depth_enable=desc->DepthStencilState.DepthEnable!=FALSE;
         o.depth_write=desc->DepthStencilState.DepthWriteMask==D3D12_DEPTH_WRITE_MASK_ALL;
         o.depth_func=static_cast<uint32_t>(desc->DepthStencilState.DepthFunc);
         o.stencil=desc->DepthStencilState.StencilEnable!=FALSE;o.so_entries=desc->StreamOutput.NumEntries;
         o.blend0=desc->BlendState.RenderTarget[0].BlendEnable!=FALSE;
         o.independent_blend=desc->BlendState.IndependentBlendEnable!=FALSE;
         o.write_mask0=desc->BlendState.RenderTarget[0].RenderTargetWriteMask;
         o.stencil_read=desc->DepthStencilState.StencilReadMask;o.stencil_write=desc->DepthStencilState.StencilWriteMask;
         o.front_func=static_cast<uint32_t>(desc->DepthStencilState.FrontFace.StencilFunc);
         o.back_func=static_cast<uint32_t>(desc->DepthStencilState.BackFace.StencilFunc);
         o.front_pass=static_cast<uint32_t>(desc->DepthStencilState.FrontFace.StencilPassOp);
         record.back_pass=uint8_t(desc->DepthStencilState.BackFace.StencilPassOp);
         {const auto& b=desc->BlendState.RenderTarget[0];
          record.blend_src=uint8_t(b.SrcBlend);record.blend_dst=uint8_t(b.DestBlend);record.blend_op=uint8_t(b.BlendOp);
          record.blend_src_alpha=uint8_t(b.SrcBlendAlpha);record.blend_dst_alpha=uint8_t(b.DestBlendAlpha);record.blend_op_alpha=uint8_t(b.BlendOpAlpha);
          // J-effects-layer: the same facts on the step-3 PsoOutput (additive fields).
          auto cf=[](D3D12_BLEND f){return f==D3D12_BLEND_BLEND_FACTOR||f==D3D12_BLEND_INV_BLEND_FACTOR||f==D3D12_BLEND_ALPHA_FACTOR||f==D3D12_BLEND_INV_ALPHA_FACTOR;};
          o.blend_constant=cf(b.SrcBlend)||cf(b.DestBlend)||cf(b.SrcBlendAlpha)||cf(b.DestBlendAlpha);
          o.blend_op0=uint8_t(b.BlendOp);o.blend_op_alpha0=uint8_t(b.BlendOpAlpha); // HUD-RS-SKIP (D3D12_BLEND_OP 1..5)
          o.rs_flare_factors=b.SrcBlend==D3D12_BLEND_ONE&&b.DestBlend==D3D12_BLEND_ONE&&b.SrcBlendAlpha==D3D12_BLEND_ONE&&b.DestBlendAlpha==D3D12_BLEND_ZERO;
          const auto& ds=desc->DepthStencilState;
          o.stencil_ops_keep=ds.FrontFace.StencilFailOp==D3D12_STENCIL_OP_KEEP&&ds.FrontFace.StencilDepthFailOp==D3D12_STENCIL_OP_KEEP&&
              ds.FrontFace.StencilPassOp==D3D12_STENCIL_OP_KEEP&&ds.BackFace.StencilFailOp==D3D12_STENCIL_OP_KEEP&&
              ds.BackFace.StencilDepthFailOp==D3D12_STENCIL_OP_KEEP&&ds.BackFace.StencilPassOp==D3D12_STENCIL_OP_KEEP;}
         {const auto& b1=desc->BlendState.RenderTarget[desc->BlendState.IndependentBlendEnable?1:0];
          record.blend1=b1.BlendEnable!=FALSE;record.write_mask1=uint8_t(b1.RenderTargetWriteMask);}}
        // J-effects-dual: for the game's exact dual-source tuple create the private clone with ONLY the alpha blend factors changed to
        // ONE / INV_SRC_ALPHA (its alpha channel then accumulates RGB coverage a+(1-a)*A). Same shaders, root signature, input layout, formats,
        // depth/stencil and rasteriser; a cached blob (it would not match) is dropped. Failure leaves dual_ready false: the producer refuses
        // such candidates (fail closed), the game's PSO is untouched either way.
        // EFFECTS-OUTSIDE (E): the explosion tuple 5,5,1/18,19,1 gets its clone too (alpha ZERO / ONE; used only when fxmem:5 admits PS 542a11d7; created with
        // the key because the explosion PSO appears at the first impact, possibly before the option arrives).
        uint32_t cloneSrcA=0,cloneDstA=0;
        if(effectsDualConfigured()&&record.output.blend0&&gpu_copy::step5::effects_clone_alpha_factors(record.blend_src,record.blend_dst,record.blend_op,
               record.blend_src_alpha,record.blend_dst_alpha,record.blend_op_alpha,record.output.write_mask0,cloneSrcA,cloneDstA)) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC clone=*desc;
            clone.CachedPSO={};
            clone.BlendState.RenderTarget[0].SrcBlendAlpha=static_cast<D3D12_BLEND>(cloneSrcA);
            clone.BlendState.RenderTarget[0].DestBlendAlpha=static_cast<D3D12_BLEND>(cloneDstA);
            // EFFECTS-OUTSIDE (P2) the additive tuple's SHARED clone is the WINDOW one: it must draw nothing (coverage_v1 cannot hold light: step162 rectangles).
            const bool additiveTuple=gpu_copy::step5::effects_additive_blend_tuple(record.blend_src,record.blend_dst,record.blend_op,record.blend_src_alpha,
                record.blend_dst_alpha,record.blend_op_alpha,record.output.write_mask0);
            if(additiveTuple) {
                clone.BlendState.RenderTarget[0].SrcBlend=static_cast<D3D12_BLEND>(gpu_copy::step5::effects_additive_window_src);
                clone.BlendState.RenderTarget[0].DestBlend=static_cast<D3D12_BLEND>(gpu_copy::step5::effects_additive_window_dst);
            }
            ID3D12PipelineState* made=nullptr;
            const auto create=creationOriginal<Fn>(self,10,true);
            if(create&&SUCCEEDED(create(self,&clone,IID_PPV_ARGS(&made)))&&made) {
                record.effectsDualClone={made,[](ID3D12PipelineState* p){p->Release();}}; // adopts the creation reference
                // EFFECTS-OUTSIDE (P) fxadd: the additive 4caf6e2d tuple is marked additive_ready, NOT dual_ready (admitted only while fxadd is on; default unchanged).
                record.output.additive_ready=gpu_copy::step5::effects_additive_blend_tuple(record.blend_src,record.blend_dst,record.blend_op,record.blend_src_alpha,
                    record.blend_dst_alpha,record.blend_op_alpha,record.output.write_mask0);
                record.output.dual_ready=!record.output.additive_ready;
                record.output.tracer_ready=gpu_copy::step5::effects_tracer_blend_tuple(record.blend_src,record.blend_dst,record.blend_op,record.blend_src_alpha,
                    record.blend_dst_alpha,record.blend_op_alpha,record.output.write_mask0); // EFFECTS-OUTSIDE (G) fxmem:6: the exact additive gun-tracer tuple (clone = the game's factors)
                // EFFECTS-OUTSIDE (I) fxmem:6: the explosion tuple also gets its per-eye "light" PSO (gpu_copy_step5.h effects_explosion_light_factors). Created
                // only after the shared clone succeeded; a failure leaves it null and the per-eye replay keeps the shared clone (named in fx-sprite-gpu xpso=).
                uint32_t lSrcA=0,lDstA=0,lOpA=0;
                if(gpu_copy::step5::effects_explosion_light_factors(record.blend_src,record.blend_dst,record.blend_op,record.blend_src_alpha,record.blend_dst_alpha,
                       record.blend_op_alpha,record.output.write_mask0,lSrcA,lDstA,lOpA)) {
                    D3D12_GRAPHICS_PIPELINE_STATE_DESC light=*desc;
                    light.CachedPSO={};
                    light.BlendState.RenderTarget[0].SrcBlendAlpha=static_cast<D3D12_BLEND>(lSrcA);
                    light.BlendState.RenderTarget[0].DestBlendAlpha=static_cast<D3D12_BLEND>(lDstA);
                    light.BlendState.RenderTarget[0].BlendOpAlpha=static_cast<D3D12_BLEND_OP>(lOpA);
                    ID3D12PipelineState* lp=nullptr;
                    if(SUCCEEDED(create(self,&light,IID_PPV_ARGS(&lp)))&&lp)record.effectsExplosionLight={lp,[](ID3D12PipelineState* p){p->Release();}};
                }
                // EFFECTS-OUTSIDE (P2) the additive tuple's per-eye "light" PSO (same slot as the explosion's: only the per-eye sprite replay binds it): RGB the game's
                // 5,2,1, alpha ZERO / ONE. A failure leaves it null; the per-eye gate then refuses the draw by name (sprite-additive-pso-missing).
                uint32_t aSrcA=0,aDstA=0;
                if(gpu_copy::step5::effects_additive_light_factors(record.blend_src,record.blend_dst,record.blend_op,record.blend_src_alpha,record.blend_dst_alpha,
                       record.blend_op_alpha,record.output.write_mask0,aSrcA,aDstA)) {
                    D3D12_GRAPHICS_PIPELINE_STATE_DESC light=*desc;
                    light.CachedPSO={};
                    light.BlendState.RenderTarget[0].SrcBlendAlpha=static_cast<D3D12_BLEND>(aSrcA);
                    light.BlendState.RenderTarget[0].DestBlendAlpha=static_cast<D3D12_BLEND>(aDstA);
                    ID3D12PipelineState* lp=nullptr;
                    if(SUCCEEDED(create(self,&light,IID_PPV_ARGS(&lp)))&&lp)record.effectsExplosionLight={lp,[](ID3D12PipelineState* p){p->Release();}};
                }
                // EFFECTS-OUTSIDE (M6) fxatlas (fx_puff_atlas.h; docs section 23): the 4caf6e2d dual tuple's CAPTURE pipeline: the GAME's root signature with
                // fx_puff_atlas.hlsl vs_copy / ps_copy (t0 = the game's table, s0 = its static sampler), RT = the atlas format, no blend, no depth. Created
                // with the dual clone (the option may arrive after the game created its PSOs); unused unless fxatlas is on. A failure leaves it null: the
                // producer then never captures for this PSO (atlas-no-copy-pso) and the M5 key path is unchanged.
#if defined(ACZ_FX_ATLAS_SHADERS)
                if(record.output.dual_ready&&record.brPs8==0x4caf6e2du&&desc->pRootSignature) {
                    const D3D12_GRAPHICS_PIPELINE_STATE_DESC cp=acz::fxpuff::atlas::copy_pso_desc(desc->pRootSignature,desc->NodeMask,g_fx_atlas_vs,sizeof g_fx_atlas_vs,
                                                                                                  g_fx_atlas_ps,sizeof g_fx_atlas_ps); // fx_puff_atlas_d3d12.h (the WARP harness runs it)
                    ID3D12PipelineState* ap=nullptr;
                    if(SUCCEEDED(create(self,&cp,IID_PPV_ARGS(&ap)))&&ap){record.effectsAtlasCopy={ap,[](ID3D12PipelineState* p){p->Release();}};record.effectsAtlasDevice=uintptr_t(self);}
                }
#endif
            }
        }
        // HUD-LATE (hudlate:1; hud_late.h, review F6): the text pipeline (VS 534f29ca / PS d34ee153, blend SRC_ALPHA / INV_SRC_ALPHA) gets a private clone with ONLY
        // the alpha blend changed to ONE / ONE / REV_SUBTRACT: a draw under it leaves alpha < 0 (the display-value marker the HUD composite reads). Failure: no clone,
        // the game's PSO is replayed (the old behaviour: text tone-mapped twice under Reinhard / exposure != 1).
        if(gpu_copy::step5::hud_late_flag().load(std::memory_order_relaxed)&&
           gpu_copy::step5::hud_late_clone_wanted(record.brVs8,record.brPs8,record.output.blend0,record.blend_src,record.blend_dst,record.output.write_mask0)) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC late=*desc;
            late.CachedPSO={};
            late.BlendState.RenderTarget[0].SrcBlendAlpha=static_cast<D3D12_BLEND>(gpu_copy::step5::kLateCloneSrcAlpha);
            late.BlendState.RenderTarget[0].DestBlendAlpha=static_cast<D3D12_BLEND>(gpu_copy::step5::kLateCloneDstAlpha);
            late.BlendState.RenderTarget[0].BlendOpAlpha=static_cast<D3D12_BLEND_OP>(gpu_copy::step5::kLateCloneOpAlpha);
            ID3D12PipelineState* lp=nullptr;
            const auto createLate=creationOriginal<Fn>(self,10,true);
            if(createLate&&SUCCEEDED(createLate(self,&late,IID_PPV_ARGS(&lp)))&&lp)record.hudLateClone={lp,[](ID3D12PipelineState* p){p->Release();}};
        }
        // drawcpt requires independent exact VS/PS/GS HASH and recomputed full
        // container SHA-256 matches, and absent HS/DS. Never hash-only facts.
        if(drawcpt) {
            record.vs_cb10_words=shader_access::e11_vs_cb10_words;
            record.ps_cb10_words=shader_access::e11_ps_cb10_words;
        }
        // E10's observed four per-vertex float4 attributes. Retain the layout
        // fact at creation; identical shaders alone do not prove IA fetches.
        record.drawcpt_ia_layout=desc->InputLayout.NumElements==4&&desc->InputLayout.pInputElementDescs;
        if(record.drawcpt_ia_layout)for(unsigned i=0;i<4;++i) {
            const auto& e=desc->InputLayout.pInputElementDescs[i];
            record.drawcpt_ia_layout&=e.SemanticName&&_stricmp(e.SemanticName,"ATTRIBUTE")==0&&e.SemanticIndex==i&&
                e.Format==DXGI_FORMAT_R32G32B32A32_FLOAT&&e.InputSlot==0&&e.AlignedByteOffset==16*i&&
                e.InputSlotClass==D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA&&!e.InstanceDataStepRate;
        }
        record.identity=capture::new_lifetime_identity();
        if(desc->GS.BytecodeLength<=capture::max_copy_bytes)
            record.shader=metadata::decode_shader_metadata(desc->GS.pShaderBytecode,desc->GS.BytecodeLength);
        ULONG rootRetainCount=0;
        record.root=retain(desc->pRootSignature,&rootRetainCount);
        const bool baseInstalled=trackMetadataLifetime(raw);
        diagnostic.conditions.set(diagnostics::base_install,baseInstalled);
        if(!baseInstalled) {
            diagnostic.pinRefusals|=PinRefusals::seams;
            auto& s=state();std::lock_guard lock(s.mutex);
            releaseConditions_locked(diagnostic.conditions,raw,false);return;
        }
        PipelineInterfaces interfaces;interfaces.observe(raw);
        diagnostic.conditions.known|=interfaces.conditions.known;
        diagnostic.conditions.value|=interfaces.conditions.value;
        record.canonical=!interfaces.canonical?PipelineRecord::Canonical::unavailable:
            interfaces.canonical==reinterpret_cast<IUnknown*>(raw)?PipelineRecord::Canonical::self:PipelineRecord::Canonical::split;
        const bool canonical=interfaces.valid;
        const bool covered=canonical&&trackMetadataLifetime(interfaces.canonical);
        bool ancestorsInstalled=canonical;
        if(canonical)for(auto* ancestor:interfaces.ancestors)if(ancestor) {
            const bool installed=trackMetadataLifetime(ancestor);
            ancestorsInstalled=installed&&ancestorsInstalled;
        }
        if(canonical)diagnostic.conditions.set(diagnostics::canonical_install,covered);
        // Unknown creation IIDs/returned aliases cannot certify that the
        // requested output was the supported base/canonical pipeline object.
        // QI success alone must not manufacture explicit-root eligibility.
        record.creation_interface_supported=(iid==__uuidof(ID3D12PipelineState)||iid==__uuidof(IUnknown))&&raw==*out;
        diagnostic.conditions.set(diagnostics::supported_output,record.creation_interface_supported);
        if(!record.creation_interface_supported)diagnostic.pinRefusals|=PinRefusals::shape;
        if(!canonical)diagnostic.pinRefusals|=PinRefusals::validation;
        if(canonical&&(!covered||!ancestorsInstalled))diagnostic.pinRefusals|=PinRefusals::seams;
        if(record.drawcpt_identity_matched&&canonical&&covered&&ancestorsInstalled&&record.creation_interface_supported&&record.identity)
            record.ownershipPin=pinCreatedPipeline(raw,record.identity,interfaces.canonical,interfaces.ancestors,
                record.root,rootRetainCount,diagnostic.pinRefusals);
        diagnostic.pin=record.ownershipPin;
        if(record.ownershipPin) {
            std::shared_ptr<ID3D12RootSignature> heldRoot;
            {auto& s=state();std::lock_guard lock(s.mutex);
             const auto& pin=s.ownershipPins[record.ownershipPin-1];
             record.ownershipIdentity=pin.identity;heldRoot=pin.root;}
            // Share the already retained root too. Balance a repeat's temporary
            // creation retain outside the mutex, while the permanent pin lives.
            record.root=std::move(heldRoot);
        }
        if(auto* log=diagnostics::sink();log&&log->ownership_pins())
            diagnostic.conditions.set(diagnostics::pinned,record.ownershipPin!=0);
        auto& s=state();
        {std::lock_guard lock(s.mutex);
         event.touch(raw);
         // Every retained QI output participates, including rejected outputs.
         // Identity validity and footprint capacity, not patch success, establish
         // a known footprint. Never overwrite a capacity refusal.
         for(const auto& ref:interfaces.refs)event.touch(ref.get());
         event.footprint.known=canonical && record.creation_interface_supported && !event.footprint.overflow;
         bool ancestorsCovered=ancestorsInstalled;
         for(size_t i=0;i<interfaces.ancestors.size();++i)if(auto* ancestor=interfaces.ancestors[i]) {
             const bool seam=releaseCovered_locked(vtable(ancestor));
             if(seam)interfaces.ancestorCovered|=1u<<i;
             ancestorsCovered=seam&&ancestorsCovered;
         }
         releaseConditions_locked(diagnostic.conditions,raw,false);
         releaseConditions_locked(diagnostic.conditions,interfaces.canonical,true);
         // Read dependency facts even when PSO validation failed. Never use
         // them to fill its explicit-root token outside the original gate.
         const auto dependencyKey=s.rootKeys.find(desc->pRootSignature);
         auto* rootKey=dependencyKey==s.rootKeys.end()?desc->pRootSignature:dependencyKey->second;
         const auto dependency=s.rootIdentities.find(rootKey);
         const auto creation=s.rootCreations.find(rootKey);
         diagnostic.conditions.set(diagnostics::dependency_accepted,
             dependency!=s.rootIdentities.end()&&dependency->second!=0);
         if(creation!=s.rootCreations.end()) {
             diagnostic.dependency=creation->second.conditions;
             if(event.serial&&creation->second.publication)
                 diagnostic.conditions.set(diagnostics::dependency_before,creation->second.publication<event.serial);
         }
         auto available=[&](IUnknown* key) {
             const auto address=reinterpret_cast<uintptr_t>(key);
             auto owner=pipelineFor_locked(key);
             if(owner!=s.pipelines.end()&&owner->first!=raw) {
                 // Conflicting per-object claims invalidate BOTH eligibility
                 // observations, including any already-bound snapshot.
                 owner->second.root_identity=0;
                 for(auto& list:s.states)if(list.second.pso.get()==owner->first) {
                     list.second.bindings.pipeline.explicit_root_identity=0;
                     ++list.second.bindings.revision;
                 }
                 return false;
             }
             return !s.listKeys.count(key)&&!s.states.count(reinterpret_cast<ID3D12GraphicsCommandList*>(key))&&
                 !s.rootKeys.count(key)&&!s.rootIdentities.count(reinterpret_cast<ID3D12RootSignature*>(key))&&
                 !s.compatibilityOwners.count(key)&&!s.nativeBindings.heap_token(address).id&&
                 !s.nativeBindings.resource_token(address).id&&
                 (owner==s.pipelines.end()||owner->first==raw);
         };
         // Canonical Release coverage AND unique per-object routing are
         // prerequisites. Shared objects/lists tables keep their sole native
         // dispatch owner. Nothing is inferred or repaired at Draw.
         event.ownershipPinned=record.ownershipPin!=0;
         event.resolveNestedReleases();
         record.ownershipRequired=event.ownershipChildren!=0;
         interfaces.format(diagnostic.detail.data(),diagnostic.detail.size(),event.orderConditions(s));
         event.appendAmbiguity(diagnostic.detail.data(),diagnostic.detail.size());
         if(canonical) {
             bool unique=available(reinterpret_cast<IUnknown*>(raw))&&available(interfaces.canonical);
             for(auto* ancestor:interfaces.ancestors)if(ancestor) {
                 const bool unclaimed=available(ancestor);unique=unclaimed&&unique;
             }
             diagnostic.conditions.set(diagnostics::unique_routing,unique);if(!unique)return;
         }
         // Distinct-event eligibility remains fail-closed; record its independent
         // diagnostic result even when Release coverage is incomplete.
         diagnostic.conditions.set(diagnostics::distinct_event,event.distinct(s));
         const bool lifetime=covered && ancestorsCovered && record.creation_interface_supported && record.identity &&
             releaseCovered_locked(vtable(raw))&&releaseCovered_locked(vtable(interfaces.canonical))&&
             (record.canonical!=PipelineRecord::Canonical::split||event.distinct(s));
         if(lifetime) {
             record.canonical_address=interfaces.canonical;
             record.ancestor_addresses=interfaces.ancestors;
             auto k=s.rootKeys.find(desc->pRootSignature);
             auto root=s.rootIdentities.find(k==s.rootKeys.end()?desc->pRootSignature:k->second);
             if(root!=s.rootIdentities.end()) record.root_identity=root->second;
         }
         // Decoding rejects unknown/newer formats without affecting creation HRESULT.
         record.conditions=diagnostic.conditions;record.dependency_conditions=diagnostic.dependency;
         record.retirementKeys=event.footprint.objects;
         record.retirementKnown=lifetime&&canonical&&record.creation_interface_supported&&
             event.footprint.known&&!event.footprint.overflow;
         record.detail=diagnostic.detail;
         record.publication=event.serial;
         s.gs[raw]=record.shader.valid && record.shader.stage==metadata::ShaderStage::geometry;
         // Replacement can release a hooked root. Move ownership out first so
         // both normal cleanup and exception unwinding run after unlocking.
         auto inserted=s.pipelines.try_emplace(raw);
         displaced=std::move(inserted.first->second);
         inserted.first->second=std::move(record);
         pipelineIndexEdit_locked(reinterpret_cast<uintptr_t>(raw),inserted.second?nullptr:&displaced,&inserted.first->second); // FPS growth fix
        }
    });
    return hr;
}
// Own the out slot before calling COM, including the supported fixture seam
// where a QI publishes a valid reference and then throws. Never under a lock.
template<class T> struct QueryReference {
    T* value=nullptr;
    QueryReference()=default;
    QueryReference(const QueryReference&)=delete;
    ~QueryReference(){if(value)value->Release();}
};
// Registration owns only temporary QI references; native output is untouched.
// `closed` (J-3b3, experiment-only CreateCommandList1 witness): the list was
// created CLOSED, so creation opens no recording. It is certified now (aliases
// and final Release covered from creation) but its first generation begins at
// its first observed Reset, exactly as for a list discovered at Reset.
void registerCreatedList(ID3D12Device* device,void* output,ID3D12PipelineState* initial,bool closed=false) {
    QueryReference<ID3D12GraphicsCommandList> owned;auto& raw=owned.value;
    if(FAILED(static_cast<IUnknown*>(output)->QueryInterface(IID_PPV_ARGS(&raw))))return;
    if(!raw)return;
    ListState displaced;
    {auto& s=state();std::lock_guard lock(s.mutex);auto key=listKey_locked(raw);
     auto prior=s.states.find(key);
     if(prior!=s.states.end()){
         if(prior->second.copyObservation)prior->second.copyObservation->uncertain("list-displaced");
         displaced=std::move(prior->second);s.states.erase(prior);}
     for(auto alias=s.listKeys.begin();alias!=s.listKeys.end();)
         if(alias->second==key)alias=s.listKeys.erase(alias);else ++alias;
    }
    if(!hookCaptureCommandList(raw))return;
    if(!closed){resetBindings(raw,true);setPSO(raw,initial);}
    auto& s=state();std::lock_guard lock(s.mutex);
    auto& current=s.states[listKey_locked(raw)];
    current.native_binding_device=reinterpret_cast<uintptr_t>(device);
    current.creation_observed=true;current.registration_path=closed?1:0;
    diagnostics::list_registered(closed?1:0);
}
HRESULT STDMETHODCALLTYPE createList(ID3D12Device* self,UINT node,D3D12_COMMAND_LIST_TYPE type,ID3D12CommandAllocator* allocator,
                                    ID3D12PipelineState* initial,REFIID iid,void** out) {
    HookAdmission admission;if(!admission)return E_FAIL;
    using Fn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,UINT,D3D12_COMMAND_LIST_TYPE,ID3D12CommandAllocator*,ID3D12PipelineState*,REFIID,void**);
    auto hr=creationOriginal<Fn>(self,12,true)(self,node,type,allocator,initial,iid,out);
    observeCreationReturn(hr,out,CreationSightings::list);
    if(SUCCEEDED(hr)&&type==D3D12_COMMAND_LIST_TYPE_DIRECT&&out&&*out)
        observeSafely([&]{registerCreatedList(self,*out,initial);});
    // Step-1 creation witness after registration (canonical route), outside locks.
    if(g_step1.load())observeSafely([&]{step1ListCreated(hr,self,node,type,allocator,out,false);});
    return hr;
}
// J-3b3: installed ONLY while the step-1 experiment owner exists, and only on
// a base device table that the device's own ID3D12Device4 interface shares
// (same object pointer, same table; see step1Device4SharesTable). A distinct
// extension table is never touched (J-056/D-86). Default OFF this slot is
// never patched and CreateCommandList1 lists are still discovered at Reset.
// The saved original is forwarded exactly once with the caller's arguments.
HRESULT STDMETHODCALLTYPE step1CreateList1(ID3D12Device4* self,UINT node,D3D12_COMMAND_LIST_TYPE type,
                                         D3D12_COMMAND_LIST_FLAGS flags,REFIID iid,void** out) {
    HookAdmission admission;if(!admission)return E_FAIL;
    using Fn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device4*,UINT,D3D12_COMMAND_LIST_TYPE,D3D12_COMMAND_LIST_FLAGS,REFIID,void**);
    const auto hr=creationOriginal<Fn>(self,51,true)(self,node,type,flags,iid,out);
    auto* device=reinterpret_cast<ID3D12Device*>(self); // same object and table (checked at install)
    observeCreationReturn(hr,out,CreationSightings::list);
    if(SUCCEEDED(hr)&&type==D3D12_COMMAND_LIST_TYPE_DIRECT&&out&&*out)
        observeSafely([&]{registerCreatedList(device,*out,nullptr,true);});
    if(g_step1.load())observeSafely([&]{step1ListCreated(hr,device,node,type,nullptr,out,true);});
    return hr;
}
// Installed ONLY while the step-1 experiment is enabled (allocator creation
// witness for the ownership registry). Allocator Release is not hooked: a new
// allocator at a still-live route is treated as a missed Release (tombstone).
HRESULT STDMETHODCALLTYPE step1CreateAllocator(ID3D12Device* self,D3D12_COMMAND_LIST_TYPE type,REFIID iid,void** out) {
    HookAdmission admission;if(!admission)return E_FAIL;
    using Fn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,D3D12_COMMAND_LIST_TYPE,REFIID,void**);
    const auto hr=creationOriginal<Fn>(self,9,true)(self,type,iid,out);
    observeSafely([&]{step1AllocatorCreated(hr,self,type,iid,out);});
    return hr;
}
// J-3b4: installed ONLY for the early-device step-1 owner (default OFF). Queue
// creation witness feeding Registry::queue_created; the saved original is
// forwarded exactly once with the caller's arguments. The queue table itself is
// hooked at swap-chain discovery as before.
HRESULT STDMETHODCALLTYPE step1CreateQueue(ID3D12Device* self,const D3D12_COMMAND_QUEUE_DESC* desc,REFIID iid,void** out) {
    HookAdmission admission;if(!admission)return E_FAIL;
    using Fn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,const D3D12_COMMAND_QUEUE_DESC*,REFIID,void**);
    const auto hr=creationOriginal<Fn>(self,8,true)(self,desc,iid,out);
    observeSafely([&]{step1QueueCreated(hr,self,desc,iid,out,0);});
    return hr;
}
// ID3D12Device9::CreateCommandQueue1, slot 75, shared base table only (see
// step1Device9SharesTable); a distinct extension table is never touched.
HRESULT STDMETHODCALLTYPE step1CreateQueue1(ID3D12Device9* self,const D3D12_COMMAND_QUEUE_DESC* desc,REFIID creator,
                                          REFIID iid,void** out) {
    HookAdmission admission;if(!admission)return E_FAIL;
    using Fn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device9*,const D3D12_COMMAND_QUEUE_DESC*,REFIID,REFIID,void**);
    const auto hr=creationOriginal<Fn>(self,75,true)(self,desc,creator,iid,out);
    auto* device=reinterpret_cast<ID3D12Device*>(self); // same object and table (checked at install)
    observeSafely([&]{step1QueueCreated(hr,device,desc,iid,out,1);});
    return hr;
}
// J-xr-stereo-output evidence: bounded startup-partition lines naming the
// creating thread, the heap/resource description and the scope/window state,
// so a live run proves who created an object the observer reacted to.
#ifdef ACZ_VR_OFFLINE_TEST
// Fixture-only copy of every formatted creation line (fixed storage, no allocation:
// several fixtures run with an exhausted allocation budget).
struct TestCreationLines {
    static constexpr unsigned capacity=256;
    char text[capacity][diagnostics::text_capacity]{};
    std::atomic<unsigned> count{0};
} g_testCreationLines;
#endif
void creationLineBudgeted(std::atomic<unsigned>& lines,unsigned cap,const char* what,const char* outcome,
    unsigned a,unsigned b,unsigned c,unsigned d,const char* reason) noexcept {
    if(lines.fetch_add(1,std::memory_order_relaxed)>=cap)return;
    char text[diagnostics::text_capacity]{};
    std::snprintf(text,sizeof text,"binding-creation;object=%s;outcome=%s;thread=%lu;a=%u;b=%u;c=%u;d=%u;xr-scope=%u;xr-window=%u;reason=%s",
        what,outcome,static_cast<unsigned long>(GetCurrentThreadId()),a,b,c,d,unsigned(proxyCreationScopeActive()),
        unsigned(xrCallWindowActive()),reason?reason:"");
#ifdef ACZ_VR_OFFLINE_TEST
    {const auto n=g_testCreationLines.count.fetch_add(1);
     if(n<TestCreationLines::capacity)std::memcpy(g_testCreationLines.text[n],text,sizeof text);}
#endif
    diagnostics::emit(diagnostics::record(diagnostics::Event::device,0,0,0,text));
}
void bindingCreationLine(const char* what,const char* outcome,unsigned a,unsigned b,unsigned c,unsigned d,const char* reason) noexcept {
    static std::atomic<unsigned> lines{0};
    creationLineBudgeted(lines,24,what,outcome,a,b,c,d,reason);
}
// J-compat-race: the compatibility-device seams (CreateSharedResource/CreateSharedHeap)
// have their OWN bounded budget so an XR swapchain's image burst (24 calls in headset
// test 2) can neither starve nor be starved by the sibling creation lines. Every line
// names the calling thread and the scope/window state, so a live run proves WHICH
// thread issued each runtime creation.
void compatCreationLine(const char* what,const char* outcome,unsigned a,unsigned b,unsigned c,unsigned d,const char* reason) noexcept {
    static std::atomic<unsigned> lines{0};
    creationLineBudgeted(lines,64,what,outcome,a,b,c,d,reason);
}
// Exact base SDK signatures and absolute slots are also checked by the fixture
// through the installed SDK's C vtable definitions and fake virtual dispatch.
HRESULT STDMETHODCALLTYPE createBindingHeap(ID3D12Device* self,const D3D12_DESCRIPTOR_HEAP_DESC* desc,REFIID iid,void** out) {
    HookAdmission admission;if(!admission)return E_FAIL;
    using Fn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,const D3D12_DESCRIPTOR_HEAP_DESC*,REFIID,void**);
    auto fn=creationOriginal<Fn>(self,14,true);
    // Temporary COM ownership is declared BEFORE the event guard, so Release
    // happens after event completion and outside every metadata/capture lock.
    std::unique_ptr<ID3D12DescriptorHeap,ReleaseCOM> owned;
    if(proxyCreationScopeActive()) {
        // No BindingEvent: a proxy-internal creation must not overlap (and so
        // disable on) a concurrent game-thread binding event.
        auto hr=fn(self,desc,iid,out);
        observeCreationReturn(hr,out,CreationSightings::descriptor_heap);
        // Proxy-internal (XR runtime / consumer) heap: forwarded natively, never
        // tracked, never a reason to disable. A stale same-address registration is dropped.
        if(SUCCEEDED(hr)&&out&&*out) {
            observeNativeBinding<diagnostics::DisableEvent::creation>([&](auto& b){b.foreign_heap(reinterpret_cast<uintptr_t>(*out));});
            bindingCreationLine("descriptor-heap","ignored-proxy-scope",desc?unsigned(desc->Type):99u,desc?unsigned(desc->Flags):99u,
                desc?desc->NodeMask:99u,desc?desc->NumDescriptors:0u,"");
        }
        return hr;
    }
    BindingEvent event(binding::ObservationKind::heap_create);
    auto hr=fn(self,desc,iid,out);
    observeCreationReturn(hr,out,CreationSightings::descriptor_heap);
    event.identify(SUCCEEDED(hr) && out?reinterpret_cast<uintptr_t>(*out):0);
    if(SUCCEEDED(hr) && out && *out && !desc) observeNativeBinding<diagnostics::DisableEvent::creation>([](auto& b){b.disable("heap creation missing description");});
    if(SUCCEEDED(hr) && desc && out && *out && (desc->Type==D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || desc->Type==D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)) {
        try {
            if(iid!=__uuidof(ID3D12DescriptorHeap)) {observeNativeBinding<diagnostics::DisableEvent::creation>([](auto& b){b.disable("heap creation IID unsupported");});return hr;}
            ID3D12DescriptorHeap* raw=nullptr;
            if(FAILED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&raw)))) {
                observeNativeBinding<diagnostics::DisableEvent::creation>([](auto& b){b.disable("heap creation QI failed");});return hr;
            }
            owned.reset(raw);
            if(raw!=*out) {observeNativeBinding<diagnostics::DisableEvent::creation>([](auto& b){b.disable("heap creation identity mismatch");});return hr;}
            auto cpu=raw->GetCPUDescriptorHandleForHeapStart();
            D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
            if(desc->Flags==D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) gpu=raw->GetGPUDescriptorHandleForHeapStart();
            auto increment=self->GetDescriptorHandleIncrementSize(desc->Type);
            if(!trackBindingLifetime(raw)) {observeNativeBinding<diagnostics::DisableEvent::creation>([](auto& b){b.disable("heap lifetime hook incomplete");});return hr;}
            uint64_t oldSerial=0;
            {std::lock_guard lock(state().mutex);
             if(const auto* d=state().nativeBindings.diagnostic()) {
                 const auto h=d->heaps.find(reinterpret_cast<uintptr_t>(raw));
                 if(h!=d->heaps.end())oldSerial=h->second.serial;
             }}
            retireSrvCandidates(oldSerial,reinterpret_cast<uintptr_t>(raw));
            const bool open=xrCallWindowOpen(),window=xrCallWindowActive();
            uint64_t tolerated=0;bool disabled=false;char rejection[96]{};
            observeNativeBinding<diagnostics::DisableEvent::creation>([&](auto& b){
                const bool was=b.enabled();const auto before=b.tolerated_rejections();
                b.heap({reinterpret_cast<uintptr_t>(raw),cpu.ptr,gpu.ptr,
                desc->NumDescriptors,increment,static_cast<uint32_t>(desc->Type),static_cast<uint32_t>(desc->Flags),desc->NodeMask},reinterpret_cast<uintptr_t>(self),window);
                tolerated=b.tolerated_rejections()-before;disabled=was&&!b.enabled();
                if(tolerated||disabled)std::snprintf(rejection,sizeof rejection,"%s",b.last_rejection());});
            if(tolerated)g_xrWindowTolerated.fetch_add(1);
            if(disabled&&open&&!window)g_xrWindowExpired.fetch_add(1);
            if(tolerated||disabled)bindingCreationLine("descriptor-heap",tolerated?"rejected-origin-unknown-xr-window":
                (open&&!window?"rejected-observer-disabled-window-expired":"rejected-observer-disabled"),
                unsigned(desc->Type),unsigned(desc->Flags),desc->NodeMask,desc->NumDescriptors,rejection);
        } catch(...) {observeNativeBinding<diagnostics::DisableEvent::creation>([](auto& b){b.disable("heap creation observation exception");});}
    }
    return hr;
}
// Shared by ordinary committed and compatibility committed-buffer observation.
bool supportedCommittedBuffer(ID3D12Device* self,const D3D12_HEAP_PROPERTIES* properties,
    D3D12_HEAP_FLAGS flags,const D3D12_RESOURCE_DESC* desc) {
    return !(!properties || flags!=D3D12_HEAP_FLAG_NONE || properties->CreationNodeMask>1 || properties->VisibleNodeMask>1 ||
               self->GetNodeCount()!=1 || desc->Layout!=D3D12_TEXTURE_LAYOUT_ROW_MAJOR || desc->Format!=DXGI_FORMAT_UNKNOWN ||
               desc->Height!=1 || desc->DepthOrArraySize!=1 || desc->MipLevels!=1 || desc->SampleDesc.Count!=1 || desc->SampleDesc.Quality);
}
HRESULT STDMETHODCALLTYPE createBindingResource(ID3D12Device* self,const D3D12_HEAP_PROPERTIES* properties,D3D12_HEAP_FLAGS flags,
    const D3D12_RESOURCE_DESC* desc,D3D12_RESOURCE_STATES initial,const D3D12_CLEAR_VALUE* clear,REFIID iid,void** out) {
    HookAdmission admission;if(!admission)return E_FAIL;
    using Fn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,const D3D12_HEAP_PROPERTIES*,D3D12_HEAP_FLAGS,
        const D3D12_RESOURCE_DESC*,D3D12_RESOURCE_STATES,const D3D12_CLEAR_VALUE*,REFIID,void**);
    auto fn=creationOriginal<Fn>(self,27,true);
    std::unique_ptr<ID3D12Resource,ReleaseCOM> owned;
    if(proxyCreationScopeActive()) {
        // Proxy-internal (e.g. XR swapchain images created inside xrCreateSwapchain
        // on the owner thread): forwarded natively, never tracked or disabling,
        // and no BindingEvent (no overlap with a concurrent game-thread event).
        auto hr=fn(self,properties,flags,desc,initial,clear,iid,out);
        observeCreationReturn(hr,out,CreationSightings::committed);
        if(SUCCEEDED(hr)&&out&&*out)bindingCreationLine("committed-resource","ignored-proxy-scope",desc?unsigned(desc->Dimension):99u,
            unsigned(flags),desc?unsigned(desc->Format):0u,properties?unsigned(properties->Type):0u,"");
        return hr;
    }
    BindingEvent event(binding::ObservationKind::committed_create);
    auto hr=fn(self,properties,flags,desc,initial,clear,iid,out);
    l3Created(hr,out,iid,desc,1,properties);
    observeCreationReturn(hr,out,CreationSightings::committed);
    if(SUCCEEDED(hr)&&out&&*out)observeNativeBinding<diagnostics::DisableEvent::creation>([&](auto& b){
        b.diagnostic_creation(0,desc?unsigned(desc->Dimension):0u);
        b.diagnostic_texture(true,reinterpret_cast<uintptr_t>(*out),reinterpret_cast<uintptr_t>(self),desc,0,properties?unsigned(properties->Type):0u,properties,unsigned(flags),&iid,
            !event.record||event.record->tainted||state().bindingEvents.load()!=1);});
    event.identify(SUCCEEDED(hr) && out?reinterpret_cast<uintptr_t>(*out):0);
    if(SUCCEEDED(hr) && out && *out && !desc) observeNativeBinding<diagnostics::DisableEvent::creation>([](auto& b){b.disable("resource creation missing description");});
    if(SUCCEEDED(hr) && desc && out && *out && desc->Dimension==D3D12_RESOURCE_DIMENSION_BUFFER) {
        try {
            if(iid!=__uuidof(ID3D12Resource)) {observeNativeBinding<diagnostics::DisableEvent::creation>([](auto& b){b.disable("resource creation IID unsupported");});return hr;}
            if(!supportedCommittedBuffer(self,properties,flags,desc)) {
                // A SHARED buffer created on another thread while a proxy XR call is in
                // flight is the runtime's: not tracked, not fatal. Game-created shared
                // buffers (no window) keep the fail-closed disable.
                if((flags&D3D12_HEAP_FLAG_SHARED)&&xrCallWindowActive()) {
                    // Counted exception, NOT ownership: the buffer's origin stays unknown
                    // (a concurrent game allocation meets the same condition), so it is
                    // tombstoned and every dependent resolution through it refuses.
                    g_xrWindowTolerated.fetch_add(1);
                    observeNativeBinding<diagnostics::DisableEvent::creation>([&](auto& b){b.mark_untracked(reinterpret_cast<uintptr_t>(*out));});
                    bindingCreationLine("committed-buffer","shared-origin-unknown-xr-window",unsigned(desc->Dimension),unsigned(flags),
                        unsigned(desc->Format),properties?unsigned(properties->Type):0u,"resource creation layout unsupported");
                    return hr;
                }
                if((flags&D3D12_HEAP_FLAG_SHARED)&&xrCallWindowOpen())g_xrWindowExpired.fetch_add(1);
                bindingCreationLine("committed-buffer","rejected-observer-disabled",unsigned(desc->Dimension),unsigned(flags),
                    unsigned(desc->Format),properties?unsigned(properties->Type):0u,"resource creation layout unsupported");
                observeNativeBinding<diagnostics::DisableEvent::creation>([](auto& b){b.disable("resource creation layout unsupported");});return hr;
            }
            ID3D12Resource* raw=nullptr;
            if(FAILED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&raw)))) {
                observeNativeBinding<diagnostics::DisableEvent::creation>([](auto& b){b.disable("resource creation QI failed");});return hr;
            }
            owned.reset(raw);
            if(raw!=*out) {observeNativeBinding<diagnostics::DisableEvent::creation>([](auto& b){b.disable("resource creation identity mismatch");});return hr;}
            auto address=raw->GetGPUVirtualAddress();
            if(!trackBindingLifetime(raw)) {observeNativeBinding<diagnostics::DisableEvent::creation>([](auto& b){b.disable("resource lifetime hook incomplete");});return hr;}
            l3Created(hr,out,iid,desc,1,properties,false);
            {auto& s=state();std::lock_guard lock(s.mutex);if(l3_locked().enabled)
                if(auto* r=s.l3.resources.get(reinterpret_cast<uintptr_t>(raw)))r->gpu_va=address;}
            observeNativeBinding<diagnostics::DisableEvent::creation>([&](auto& b){b.resource({reinterpret_cast<uintptr_t>(raw),address,desc->Width,1,false},reinterpret_cast<uintptr_t>(self));bindUploadCreation_locked(reinterpret_cast<uintptr_t>(raw));});
            pinUploadCreation(hr,out,iid,desc,properties,true,event.record);
            {auto& s=state();std::lock_guard lock(s.mutex);bindUploadCreation_locked(reinterpret_cast<uintptr_t>(raw));}
        } catch(...) {observeNativeBinding<diagnostics::DisableEvent::creation>([](auto& b){b.disable("resource creation observation exception");});}
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE unsupportedPlaced(ID3D12Device* self,ID3D12Heap* heap,UINT64 offset,const D3D12_RESOURCE_DESC* desc,
    D3D12_RESOURCE_STATES initial,const D3D12_CLEAR_VALUE* clear,REFIID iid,void** out) {
    HookAdmission admission;if(!admission)return E_FAIL;
    using Fn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,ID3D12Heap*,UINT64,const D3D12_RESOURCE_DESC*,D3D12_RESOURCE_STATES,
                                        const D3D12_CLEAR_VALUE*,REFIID,void**);
    if(proxyCreationScopeActive()) { // J-xr-stereo-output: proxy-internal, unobserved
        auto hr=creationOriginal<Fn>(self,29,true)(self,heap,offset,desc,initial,clear,iid,out);
        observeCreationReturn(hr,out,CreationSightings::placed);
        if(SUCCEEDED(hr)&&out&&*out)bindingCreationLine("placed-resource","ignored-proxy-scope",desc?unsigned(desc->Dimension):99u,0,0,0,"");
        return hr;
    }
    BindingEvent event(binding::ObservationKind::placed_create);
    auto hr=creationOriginal<Fn>(self,29,true)(self,heap,offset,desc,initial,clear,iid,out);
    l3Created(hr,out,iid,desc,2);
    pinUploadCreation(hr,out,iid,desc,nullptr);
    observeCreationReturn(hr,out,CreationSightings::placed);
    if(SUCCEEDED(hr)&&out&&*out)observeNativeBinding<diagnostics::DisableEvent::creation>([&](auto& b){
        b.diagnostic_creation(1,desc?unsigned(desc->Dimension):0u);
        b.diagnostic_texture(true,reinterpret_cast<uintptr_t>(*out),reinterpret_cast<uintptr_t>(self),desc,1,0,nullptr,0,&iid,!event.record||event.record->tainted||state().bindingEvents.load()!=1);});
    event.identify(SUCCEEDED(hr) && out?reinterpret_cast<uintptr_t>(*out):0);
    if(SUCCEEDED(hr) && out && *out && desc && desc->Dimension==D3D12_RESOURCE_DIMENSION_BUFFER) {
        if(proxyCreationScopeActive())bindingCreationLine("placed-buffer","ignored-proxy-scope",1,0,0,0,"");
        else observeNativeBinding<diagnostics::DisableEvent::creation>([](auto& b){b.disable("placed buffer unsupported");});
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE unsupportedReserved(ID3D12Device* self,const D3D12_RESOURCE_DESC* desc,
    D3D12_RESOURCE_STATES initial,const D3D12_CLEAR_VALUE* clear,REFIID iid,void** out) {
    HookAdmission admission;if(!admission)return E_FAIL;
    using Fn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,const D3D12_RESOURCE_DESC*,D3D12_RESOURCE_STATES,
                                        const D3D12_CLEAR_VALUE*,REFIID,void**);
    if(proxyCreationScopeActive()) { // J-xr-stereo-output: proxy-internal, unobserved
        auto hr=creationOriginal<Fn>(self,30,true)(self,desc,initial,clear,iid,out);
        observeCreationReturn(hr,out,CreationSightings::reserved);
        if(SUCCEEDED(hr)&&out&&*out)bindingCreationLine("reserved-resource","ignored-proxy-scope",desc?unsigned(desc->Dimension):99u,0,0,0,"");
        return hr;
    }
    BindingEvent event(binding::ObservationKind::reserved_create);
    auto hr=creationOriginal<Fn>(self,30,true)(self,desc,initial,clear,iid,out);
    l3Created(hr,out,iid,desc,3);
    pinUploadCreation(hr,out,iid,desc,nullptr);
    observeCreationReturn(hr,out,CreationSightings::reserved);
    if(SUCCEEDED(hr)&&out&&*out)observeNativeBinding<diagnostics::DisableEvent::creation>([&](auto& b){
        b.diagnostic_creation(2,desc?unsigned(desc->Dimension):0u);
        b.diagnostic_texture(true,reinterpret_cast<uintptr_t>(*out),reinterpret_cast<uintptr_t>(self),desc,2,0,nullptr,0,&iid,!event.record||event.record->tainted||state().bindingEvents.load()!=1);});
    event.identify(SUCCEEDED(hr) && out?reinterpret_cast<uintptr_t>(*out):0);
    if(SUCCEEDED(hr) && out && *out && desc && desc->Dimension==D3D12_RESOURCE_DIMENSION_BUFFER) {
        if(proxyCreationScopeActive())bindingCreationLine("reserved-buffer","ignored-proxy-scope",1,0,0,0,"");
        else observeNativeBinding<diagnostics::DisableEvent::creation>([](auto& b){b.disable("reserved buffer unsupported");});
    }
    return hr;
}
void STDMETHODCALLTYPE writeBindingSrv(ID3D12Device* self,ID3D12Resource* resource,const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    HookAdmission admission;if(!admission)return;
    using Fn=void(STDMETHODCALLTYPE*)(ID3D12Device*,ID3D12Resource*,const D3D12_SHADER_RESOURCE_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE);
    if(proxyCreationScopeActive()){creationOriginal<Fn>(self,18,true)(self,resource,desc,dest);return;} // J-xr-stereo-output: proxy-internal, unobserved
    BindingEvent event(0,true,reinterpret_cast<uintptr_t>(resource),self,1,&dest);
    event.prepareCandidates();
    // Reuse the existing entry footprint; no new heap/resource scan or allocation.
    const auto* log=diagnostics::sink();
    const auto* range=event.record&&event.record->ranges.count==1?event.record->ranges.begin():nullptr;
    const bool read=log&&log->ownership_pins()&&!desc&&range&&range->slots&&
        range->slots.type==D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV&&
        (range->slots.flags&D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
    // Before forwarding: caller owns pResource throughout this call. No hook mutex.
    const auto view_write=diagnostics::ViewWriteDescription::read(read,resource);
    creationOriginal<Fn>(self,18,true)(self,resource,desc,dest);
    diagnostics::emit(diagnostics::record(diagnostics::Event::descriptor,1,0,0,"SRV metadata write"));
    SrvCandidates::Reservation candidate;
    observeNativeBinding<diagnostics::DisableEvent::descriptor_write>([&](auto& b){
        b.srv(reinterpret_cast<uintptr_t>(resource),desc,dest.ptr,reinterpret_cast<uintptr_t>(self),&view_write);
        if(view_write.observed)l3_locked().view(view_write.address,view_write.desc);
        // Reuse the entry footprint's heap/slot; no extra heap scan.
        // Diagnostic publication/reservation are atomic under the proxy mutex.
        const auto* d=b.diagnostic();
        if(log&&log->ownership_pins()&&d&&range&&range->slots) { // U-soak: not gated on shared-log I/O health
            const auto h=d->heaps.find(range->slots.heap.id);
            if(h!=d->heaps.end()&&h->second.live) {
                const auto serial=h->second.serial;const auto slot=range->slots.first;
                const auto v=d->slots.find({serial,slot});
                if(read&&view_write.observed&&!view_write.reasons&&v!=d->slots.end()&&
                   !d->overflow&&d->history_complete&&!d->gap&&!event.record->tainted&&
                   state().bindingEvents.load()==1&&v->second.ordinal&&
                   v->second.ordinal==v->second.view_write.ordinal&&v->second.desc==1&&
                   v->second.view_write.address==reinterpret_cast<uintptr_t>(resource))
                    candidate=state().srvCandidates.reserve({reinterpret_cast<uintptr_t>(resource),
                        serial,v->second.ordinal,slot});
            }
        }
    });
    // AddRef only the live API argument. Never dispatch COM at Draw.
    if(candidate.slot<SrvCandidates::capacity) {
        ULONG refs=0;bool fault=false;
        try {refs=resource->AddRef();}catch(...) {fault=true;}
        ID3D12Resource* cancelled=nullptr;
        {std::lock_guard lock(state().mutex);
         cancelled=state().srvCandidates.finish(candidate,refs?resource:nullptr,fault);
         srvCandidateCounts_locked();}
        if(cancelled){candidate.evicted=cancelled;releaseSrvCandidate(candidate);}
    }
}
void STDMETHODCALLTYPE writeBindingCbv(ID3D12Device* self,const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    HookAdmission admission;if(!admission)return;
    using Fn=void(STDMETHODCALLTYPE*)(ID3D12Device*,const D3D12_CONSTANT_BUFFER_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE);
    if(proxyCreationScopeActive()){creationOriginal<Fn>(self,17,true)(self,desc,dest);return;} // J-xr-stereo-output: proxy-internal, unobserved
    BindingEvent event(0,true,0,self,1,&dest);
    event.prepareCandidates();
    creationOriginal<Fn>(self,17,true)(self,desc,dest);
    diagnostics::emit(diagnostics::record(diagnostics::Event::descriptor,1,0,0,"CBV metadata write"));
    observeNativeBinding<diagnostics::DisableEvent::descriptor_write>([&](auto& b){b.cbv(desc,dest.ptr,reinterpret_cast<uintptr_t>(self));});
}
void STDMETHODCALLTYPE writeBindingUav(ID3D12Device* self,ID3D12Resource* resource,ID3D12Resource* counter,
    const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    HookAdmission admission;if(!admission)return;
    using Fn=void(STDMETHODCALLTYPE*)(ID3D12Device*,ID3D12Resource*,ID3D12Resource*,const D3D12_UNORDERED_ACCESS_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE);
    if(proxyCreationScopeActive()){creationOriginal<Fn>(self,19,true)(self,resource,counter,desc,dest);return;} // J-xr-stereo-output: proxy-internal, unobserved
    BindingEvent event(0,true,reinterpret_cast<uintptr_t>(resource),self,1,&dest);
    event.prepareCandidates();
    creationOriginal<Fn>(self,19,true)(self,resource,counter,desc,dest);
    diagnostics::emit(diagnostics::record(diagnostics::Event::descriptor,1,0,0,"unsupported UAV metadata write"));
    observeNativeBinding([&](auto& b){b.diagnostic_view(reinterpret_cast<uintptr_t>(resource),reinterpret_cast<uintptr_t>(self));
        b.diagnostic_view(reinterpret_cast<uintptr_t>(counter),reinterpret_cast<uintptr_t>(self));
        b.unsupported_write(dest.ptr,reinterpret_cast<uintptr_t>(self),diagnostics::DescriptorTableDiagnostics::uav);});
}
// J-3b7b (step 3 only): the texture's own GetDesc/GetHeapProperties (never
// patched by this proxy) after the native view creation, outside every lock;
// then one bounded table update under the hook-state mutex. Diagnostic only.
// J-world-evidence: the RTV description the game passed (format, dimension, mip, first array slice); data only.
inline HookState::Step3ViewDesc step3ViewDescOf(const D3D12_RENDER_TARGET_VIEW_DESC* d) noexcept {
    HookState::Step3ViewDesc v;if(!d)return v;
    v.present=true;v.format=static_cast<uint32_t>(d->Format);v.dim=static_cast<uint32_t>(d->ViewDimension);
    switch(d->ViewDimension) {
    case D3D12_RTV_DIMENSION_TEXTURE2D:v.mip=d->Texture2D.MipSlice;break;
    case D3D12_RTV_DIMENSION_TEXTURE2DARRAY:v.mip=d->Texture2DArray.MipSlice;v.slice=d->Texture2DArray.FirstArraySlice;break;
    default:break;
    }
    return v;
}
inline HookState::Step3ViewDesc step3ViewDescOf(const D3D12_DEPTH_STENCIL_VIEW_DESC*) noexcept {return HookState::Step3ViewDesc{};}
__declspec(noinline) void step3NoteView(ID3D12Resource* resource,SIZE_T handle,bool depth,bool sub0=true,bool nullDesc=true,const HookState::Step3ViewDesc* vd=nullptr) noexcept {
    HookState::Step3View entry{handle,reinterpret_cast<uintptr_t>(resource),depth};entry.sub0=sub0;if(vd)entry.vd=*vd;
    if(resource)try {
        const auto d=resource->GetDesc();entry.width=d.Width;entry.height=d.Height;entry.format=static_cast<uint32_t>(d.Format);
        entry.samples=d.SampleDesc.Count;entry.flags=static_cast<uint32_t>(d.Flags);entry.dim=static_cast<uint32_t>(d.Dimension);
        // J-3b9c (review 2, P2): a NULL-description RTV covers mip 0 of ALL array slices: it names
        // subresource 0 only for a single-slice, non-array TEXTURE2D.
        if(nullDesc)entry.sub0=d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&d.DepthOrArraySize==1;
        D3D12_HEAP_PROPERTIES hp{};D3D12_HEAP_FLAGS hf{};
        if(SUCCEEDED(resource->GetHeapProperties(&hp,&hf)))entry.heap=static_cast<uint32_t>(hp.Type);
        entry.described=true;
        if(!depth&&d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&(d.Flags&D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) {
            auto& s=state();std::lock_guard lock(s.mutex);
            step5TrackWorldLifetime_locked(resource,false);
        }
    } catch(...) {entry.described=false;}
    auto& s=state();std::lock_guard lock(s.mutex);
    entry.serial=++s.step3ViewSerial;
    for(auto& v:s.step3Views)if(v.handle&&v.handle==handle){v=entry;return;}
    s.step3Views[s.step3ViewNext++%s.step3Views.size()]=entry;
}
// J-3b9: a descriptor COPY into an RTV/DSV heap: the bind-time contents of every
// OM binding recorded before it are no longer proved (conservative global epoch).
void step3NoteDescriptorCopy(D3D12_DESCRIPTOR_HEAP_TYPE type) noexcept {
    if(type!=D3D12_DESCRIPTOR_HEAP_TYPE_RTV&&type!=D3D12_DESCRIPTOR_HEAP_TYPE_DSV)return;
    auto& s=state();std::lock_guard lock(s.mutex);++s.step3CopyEpoch;
}
// J-3b9: a null desc or a TEXTURE2D view of mip 0 / plane 0 names subresource 0.
inline bool step3ViewSub0(const D3D12_RENDER_TARGET_VIEW_DESC* d) noexcept {
    return !d||(d->ViewDimension==D3D12_RTV_DIMENSION_TEXTURE2D&&d->Texture2D.MipSlice==0&&d->Texture2D.PlaneSlice==0);
}
inline bool step3ViewSub0(const D3D12_DEPTH_STENCIL_VIEW_DESC* d) noexcept {
    return !d||(d->ViewDimension==D3D12_DSV_DIMENSION_TEXTURE2D&&d->Texture2D.MipSlice==0);
}
template<int Slot,class Desc> void STDMETHODCALLTYPE unsupportedView(ID3D12Device* self,ID3D12Resource* resource,const Desc* desc,D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    HookAdmission admission;if(!admission)return;
    using Fn=void(STDMETHODCALLTYPE*)(ID3D12Device*,ID3D12Resource*,const Desc*,D3D12_CPU_DESCRIPTOR_HANDLE);
    if(proxyCreationScopeActive()){creationOriginal<Fn>(self,Slot,true)(self,resource,desc,dest);return;} // J-xr-stereo-output: proxy-internal, unobserved
    BindingEvent event(0,true,0,self,1,&dest);
    event.prepareCandidates();
    creationOriginal<Fn>(self,Slot,true)(self,resource,desc,dest);
    // J-3b7b: remember which resource a view handle names (step 3 only; data only).
    if(step3Mode()){const HookState::Step3ViewDesc vd=step3ViewDescOf(desc);step3NoteView(resource,dest.ptr,Slot==21,step3ViewSub0(desc),desc==nullptr,&vd);}
    diagnostics::emit(diagnostics::record(diagnostics::Event::descriptor,1,0,0,"unsupported view metadata write"));
    observeNativeBinding<diagnostics::DisableEvent::descriptor_write>([&](auto& b){b.diagnostic_view(reinterpret_cast<uintptr_t>(resource),reinterpret_cast<uintptr_t>(self));
        b.unsupported_write(dest.ptr,reinterpret_cast<uintptr_t>(self));});
}
void STDMETHODCALLTYPE unsupportedSampler(ID3D12Device* self,const D3D12_SAMPLER_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    HookAdmission admission;if(!admission)return;
    using Fn=void(STDMETHODCALLTYPE*)(ID3D12Device*,const D3D12_SAMPLER_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE);
    if(proxyCreationScopeActive()){creationOriginal<Fn>(self,22,true)(self,desc,dest);return;} // J-xr-stereo-output: proxy-internal, unobserved
    BindingEvent event(0,true,0,self,1,&dest);
    event.prepareCandidates();
    creationOriginal<Fn>(self,22,true)(self,desc,dest);
    diagnostics::emit(diagnostics::record(diagnostics::Event::descriptor,1,0,0,"sampler metadata write"));
    observeNativeBinding<diagnostics::DisableEvent::descriptor_write>([&](auto& b){b.sampler(desc,dest.ptr,reinterpret_cast<uintptr_t>(self));});
}
void STDMETHODCALLTYPE copyBindingDescriptors(ID3D12Device* self,UINT dc,const D3D12_CPU_DESCRIPTOR_HANDLE* dest,const UINT* ds,
    UINT sc,const D3D12_CPU_DESCRIPTOR_HANDLE* source,const UINT* ss,D3D12_DESCRIPTOR_HEAP_TYPE type) {
    HookAdmission admission;if(!admission)return;
    using Fn=void(STDMETHODCALLTYPE*)(ID3D12Device*,UINT,const D3D12_CPU_DESCRIPTOR_HANDLE*,const UINT*,UINT,
                                    const D3D12_CPU_DESCRIPTOR_HANDLE*,const UINT*,D3D12_DESCRIPTOR_HEAP_TYPE);
    if(proxyCreationScopeActive()){creationOriginal<Fn>(self,23,true)(self,dc,dest,ds,sc,source,ss,type);return;} // J-xr-stereo-output: proxy-internal, unobserved
    BindingEvent event(0,true,0,self,dc,dest,ds,sc,source,ss,binding::ObservationKind::copy,static_cast<int>(type));
    event.prepareCandidates();
    creationOriginal<Fn>(self,23,true)(self,dc,dest,ds,sc,source,ss,type);
    if(step3Mode())step3NoteDescriptorCopy(type);
    diagnostics::emit(diagnostics::record(diagnostics::Event::descriptor,dc,sc,0,"descriptor range copy metadata"));
    observeNativeBinding<diagnostics::DisableEvent::descriptor_copy>([&](auto& b){b.copy(dc,dest,ds,sc,source,ss,type,reinterpret_cast<uintptr_t>(self));});
}
void STDMETHODCALLTYPE copyBindingSimple(ID3D12Device* self,UINT count,D3D12_CPU_DESCRIPTOR_HANDLE dest,
    D3D12_CPU_DESCRIPTOR_HANDLE source,D3D12_DESCRIPTOR_HEAP_TYPE type) {
    HookAdmission admission;if(!admission)return;
    using Fn=void(STDMETHODCALLTYPE*)(ID3D12Device*,UINT,D3D12_CPU_DESCRIPTOR_HANDLE,D3D12_CPU_DESCRIPTOR_HANDLE,D3D12_DESCRIPTOR_HEAP_TYPE);
    if(proxyCreationScopeActive()){creationOriginal<Fn>(self,24,true)(self,count,dest,source,type);return;} // J-xr-stereo-output: proxy-internal, unobserved
    BindingEvent event(0,true,0,self,1,&dest,&count,1,&source,&count,binding::ObservationKind::copy,static_cast<int>(type));
    event.prepareCandidates();
    creationOriginal<Fn>(self,24,true)(self,count,dest,source,type);
    if(step3Mode())step3NoteDescriptorCopy(type);
    diagnostics::emit(diagnostics::record(diagnostics::Event::descriptor,count,0,0,"simple descriptor copy metadata"));
    observeNativeBinding<diagnostics::DisableEvent::descriptor_copy>([&](auto& b){b.copy(1,&dest,&count,1,&source,&count,type,reinterpret_cast<uintptr_t>(self),true);});
}
// Compatibility is deliberately separate from the read-only companion list.
// SDK 10.0.26100.0 um/d3d12compatibility.h:202-252 C vtable:
// QI/AddRef/Release=0/1/2, CreateSharedResource=3, CreateSharedHeap=4,
// ReflectSharedProperties=5. Signatures: .h:172-195 / .idl:38-62.
// ReflectSharedProperties remains the native slot, without observation.
template<class F> F compatibilityOriginal(void* self,int slot) {
    auto& s=state();std::lock_guard lock(s.mutex);
    auto it=s.compatibility.find(vtable(self));
    return it==s.compatibility.end()?nullptr:reinterpret_cast<F>(it->second.originals[slot]);
}
ID3D12Device* compatibilityDevice(IUnknown* self) {
    auto& s=state();std::lock_guard lock(s.mutex);
    auto it=s.compatibilityOwners.find(self);
    return it==s.compatibilityOwners.end()?nullptr:it->second.device;
}
// Nine static per-call reasons, plus one accepted record literal. No per-call
// reason is an observer-disable reason; diagnostics owns a bounded copied text.
void untrackedCompatibility(uint64_t object,const char* reason) noexcept {
    observeNativeBinding<diagnostics::DisableEvent::creation>([&](auto& b){b.mark_untracked(object);});
    diagnostics::emit(diagnostics::record(diagnostics::Event::device,0,0,0,reason));
}
HRESULT STDMETHODCALLTYPE createCompatibilityResource(ID3D12CompatibilityDevice* self,
    const D3D12_HEAP_PROPERTIES* properties,D3D12_HEAP_FLAGS flags,const D3D12_RESOURCE_DESC* desc,
    D3D12_RESOURCE_STATES initial,const D3D12_CLEAR_VALUE* clear,const D3D11_RESOURCE_FLAGS* flags11,
    D3D12_COMPATIBILITY_SHARED_FLAGS compatibilityFlags,ID3D12LifetimeTracker* lifetime,
    ID3D12SwapChainAssistant* swapchain,REFIID iid,void** out) {
    HookAdmission admission;if(!admission)return E_FAIL;
    using Fn=decltype(&createCompatibilityResource);
    auto fn=compatibilityOriginal<Fn>(self,3);
    // J-compat-race (headset test 2): SteamVR's OpenXR runtime creates its swapchain
    // images through this seam inside xrCreateSwapchain on the XR owner thread. Like
    // every sibling creation hook, a proxy-internal call is forwarded natively ONCE and
    // never observed: no BindingEvent (so no unknown-footprint overlap with a concurrent
    // game-thread descriptor write can disable the observer), no L3/step-5 creation
    // record, no tracking or tombstone, no COM call (QI/AddRef) on the runtime's object.
    if(proxyCreationScopeActive()) {
        auto hr=fn(self,properties,flags,desc,initial,clear,flags11,compatibilityFlags,lifetime,swapchain,iid,out);
        observeCreationReturn(hr,out,CreationSightings::compatibility_resource);
        compatCreationLine("compat-resource","ignored-proxy-scope",desc?unsigned(desc->Dimension):99u,unsigned(flags),
            desc?unsigned(desc->Format):0u,unsigned(compatibilityFlags),SUCCEEDED(hr)&&out&&*out?"":"native-failed-or-null");
        return hr;
    }
    auto device=compatibilityDevice(self);
    std::unique_ptr<ID3D12Resource,ReleaseCOM> owned;
    std::unique_ptr<IUnknown,ReleaseCOM> canonicalOwner;
    // J-compat-race (thread unproven): if the runtime issues the same calls from ITS
    // OWN thread while the owner is inside a window call (xrCreateSwapchain/
    // xrCreateSession, bounded by kXrWindowCapMs), the scope test above is false.
    // Exempt ONLY forms whose arguments alone already force the "unsupported form"
    // rejection below (texture/flags/D3D11 flags/lifetime tracker/swapchain
    // assistant): such an object is tombstoned (origin-unknown, never tracked) on
    // every path, so its cohort event carries no metadata a descriptor write could
    // conflict with. Everything else below is unchanged (L3 record, QI, tombstone);
    // only the cohort event is not opened. A trackable buffer form, or any form
    // outside the window, keeps the event and the fail-closed overlap policy.
    const bool argumentsUnsupported=!desc || desc->Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER ||
        desc->Flags!=D3D12_RESOURCE_FLAG_NONE || compatibilityFlags!=D3D12_COMPATIBILITY_SHARED_FLAG_NONE ||
        flags11 || lifetime || swapchain;
    const bool windowExempt=argumentsUnsupported && xrCallWindowActive();
    std::optional<BindingEvent> event;
    if(!windowExempt)event.emplace(binding::ObservationKind::committed_create);
    HRESULT hr;
    try {hr=fn(self,properties,flags,desc,initial,clear,flags11,compatibilityFlags,lifetime,swapchain,iid,out);}
    catch(...) {
        // Output is not established after an exception. Preserve native throw;
        // the unknown cohort footprint retains the existing overlap policy.
        observeNativeBinding<diagnostics::DisableEvent::creation>([](auto& b){b.disable("resource creation observation exception");});
        untrackedCompatibility(0,"compat resource: native exception");throw;
    }
    observeCreationReturn(hr,out,CreationSightings::compatibility_resource);
    l3Created(hr,out,iid,desc,1,properties);
    if(SUCCEEDED(hr)&&out&&*out)observeNativeBinding<diagnostics::DisableEvent::creation>([&](auto& b){
        b.diagnostic_creation(3,desc?unsigned(desc->Dimension):0u);});
    const auto object=out?reinterpret_cast<uintptr_t>(*out):0; // failed creation still tombstones a non-null output
    if(event)event->identify(object);
    if(windowExempt)g_xrWindowTolerated.fetch_add(1);
    compatCreationLine("compat-resource",windowExempt?"untracked-origin-unknown-xr-window":"observed",
        desc?unsigned(desc->Dimension):99u,unsigned(flags),desc?unsigned(desc->Format):0u,unsigned(compatibilityFlags),
        argumentsUnsupported?"arguments-unsupported":"");
    if(FAILED(hr) || !object) {
        untrackedCompatibility(object,"compat resource: failed or null output");return hr;
    }
    IUnknown* canonical=nullptr;
    auto reject=[&](const char* reason) noexcept {
        if(canonical && reinterpret_cast<uintptr_t>(canonical)!=object)
            untrackedCompatibility(reinterpret_cast<uintptr_t>(canonical),reason);
        untrackedCompatibility(object,reason);
    };
    try {
        // Even unsupported descriptions/IIDs can return an alias of an already
        // tracked resource. Quarantine its canonical address as well as its
        // returned interface; keep the temporary COM reference outside locks.
        const auto canonicalHr=static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&canonical));
        canonicalOwner.reset(canonical);
        if(!device || !desc || desc->Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER ||
           !supportedCommittedBuffer(device,properties,flags,desc) || desc->Flags!=D3D12_RESOURCE_FLAG_NONE ||
           compatibilityFlags!=D3D12_COMPATIBILITY_SHARED_FLAG_NONE || flags11 || lifetime || swapchain) {
            reject("compat resource: unsupported form");return hr;
        }
        if(iid!=__uuidof(ID3D12Resource)) {
            reject("compat resource: unsupported IID");return hr;
        }
        ID3D12Resource* raw=nullptr;
        auto queryHr=static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&raw));owned.reset(raw);
        if(FAILED(queryHr) || !raw || raw!=*out || FAILED(canonicalHr) || canonical!=raw) {
            if(raw)untrackedCompatibility(reinterpret_cast<uintptr_t>(raw),"compat resource: identity unprovable");
            reject("compat resource: identity unprovable");return hr;
        }
        bool alias=false;
        {auto& s=state();std::lock_guard lock(s.mutex);
         alias=s.nativeBindings.untracked(object) || s.nativeBindings.resource_token(object).id || s.nativeBindings.heap_token(object).id;}
        if(alias) {reject("compat resource: existing object alias");return hr;}
        auto address=raw->GetGPUVirtualAddress();
        if(!address || !desc->Width || desc->Width>UINT64_MAX-address) {
            reject("compat resource: unsupported form");return hr;
        }
        if(!trackBindingLifetime(raw)) {
            // An incomplete lifetime patch is a global missed-event risk, not
            // merely an unsupported argument form. Retained originals forward.
            observeNativeBinding<diagnostics::DisableEvent::creation>([](auto& b){b.disable("resource lifetime hook incomplete");});
            reject("compat resource: lifetime hook incomplete");return hr;
        }
        l3Created(hr,out,iid,desc,1,properties,false);
            {auto& s=state();std::lock_guard lock(s.mutex);if(l3_locked().enabled)
                if(auto* r=s.l3.resources.get(reinterpret_cast<uintptr_t>(raw)))r->gpu_va=address;}
        bool publicationAlias=false;
        observeNativeBinding<diagnostics::DisableEvent::creation>([&](auto& b){
            // Check again under the publication lock: concurrent same-address
            // creations must not replace one another's provenance.
            if(b.untracked(object) || b.resource_token(object).id || b.heap_token(object).id) {
                publicationAlias=true;b.mark_untracked(object);
            }
            else {b.resource({object,address,desc->Width,1,false},reinterpret_cast<uintptr_t>(device));bindUploadCreation_locked(object);}
        });
        if(publicationAlias)reject("compat resource: existing object alias");
        else {
            pinUploadCreation(hr,out,iid,desc,properties,true,event?event->record:nullptr); // exempt forms never reach here
            auto& s=state();std::lock_guard lock(s.mutex);bindUploadCreation_locked(object);
        }
    } catch(...) {reject("compat resource: observation exception");}
    return hr;
}
HRESULT STDMETHODCALLTYPE createCompatibilityHeap(ID3D12CompatibilityDevice* self,const D3D12_HEAP_DESC* desc,
    D3D12_COMPATIBILITY_SHARED_FLAGS flags,REFIID iid,void** out) {
    HookAdmission admission;if(!admission)return E_FAIL;
    using Fn=decltype(&createCompatibilityHeap);
    // J-compat-race: proxy-internal (XR owner scope) creation, forwarded once and never
    // observed, exactly like createCompatibilityResource above.
    if(proxyCreationScopeActive()) {
        auto hr=compatibilityOriginal<Fn>(self,4)(self,desc,flags,iid,out);
        observeCreationReturn(hr,out,CreationSightings::compatibility_heap);
        compatCreationLine("compat-heap","ignored-proxy-scope",desc?unsigned(desc->Properties.Type):99u,
            desc?unsigned(desc->Flags):0u,unsigned(flags),0u,SUCCEEDED(hr)&&out&&*out?"":"native-failed-or-null");
        return hr;
    }
    std::unique_ptr<IUnknown,ReleaseCOM> canonicalOwner;
    // Every ID3D12Heap form is unprovable and tombstoned below, so inside an active XR
    // window (another thread) only the cohort event is skipped; outside it, unchanged.
    const bool windowExempt=xrCallWindowActive();
    std::optional<BindingEvent> event;
    if(!windowExempt)event.emplace(binding::ObservationKind::heap_create);
    HRESULT hr;
    try {hr=compatibilityOriginal<Fn>(self,4)(self,desc,flags,iid,out);}
    catch(...) {
        observeNativeBinding<diagnostics::DisableEvent::creation>([](auto& b){b.disable("heap creation observation exception");});
        untrackedCompatibility(0,"compat heap: unsupported memory heap");throw;
    }
    observeCreationReturn(hr,out,CreationSightings::compatibility_heap);
    const auto object=out?reinterpret_cast<uintptr_t>(*out):0;
    if(event)event->identify(object);
    if(windowExempt)g_xrWindowTolerated.fetch_add(1);
    compatCreationLine("compat-heap",windowExempt?"untracked-origin-unknown-xr-window":"observed",
        desc?unsigned(desc->Properties.Type):99u,desc?unsigned(desc->Flags):0u,unsigned(flags),0u,"");
    // D3D12_HEAP_DESC/ID3D12Heap is not a descriptor heap. The existing
    // HeapEvent schema cannot represent it; all forms remain unprovable.
    if(SUCCEEDED(hr) && object) {
        IUnknown* canonical=nullptr;
        try {
            static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&canonical));
            canonicalOwner.reset(canonical);
            if(canonical && reinterpret_cast<uintptr_t>(canonical)!=object)
                untrackedCompatibility(reinterpret_cast<uintptr_t>(canonical),"compat heap: unsupported memory heap");
        } catch(...) {
            // An exceptional identity probe is a missed observation.
            observeNativeBinding<diagnostics::DisableEvent::creation>([](auto& b){b.disable("heap creation observation exception");});
        }
    }
    untrackedCompatibility(object,"compat heap: unsupported memory heap");return hr;
}
HRESULT STDMETHODCALLTYPE queryCompatibilityDevice(ID3D12CompatibilityDevice*,REFIID,void**);
ULONG STDMETHODCALLTYPE releaseCompatibilityDevice(ID3D12CompatibilityDevice* self) {
    HookAdmission admission;if(!admission)return 0;
    using Fn=decltype(&releaseCompatibilityDevice);
    capture::Id token=0;
    {auto& s=state();std::lock_guard lock(s.mutex);auto it=s.compatibilityOwners.find(self);
     if(it!=s.compatibilityOwners.end())token=it->second.token;}
    auto refs=compatibilityOriginal<Fn>(self,2)(self);
    // J-compat-race: mirrors releaseBindingDevice. A non-final runtime Release inside an
    // XR call publishes nothing (no event, no owner change). A FINAL release still fails
    // closed below, in or out of the scope.
    if(refs&&proxyCreationScopeActive())return refs;
    if(!refs) {
        BindingEvent event(reinterpret_cast<uintptr_t>(self),true,binding::ObservationKind::device_release);
        observeNativeBinding<diagnostics::DisableEvent::release>([&](auto& b){
            auto& owners=state().compatibilityOwners;auto it=owners.find(self);
            if(it!=owners.end() && it->second.token==token)owners.erase(it);
            b.disable("binding device final release");
        });
        diagnostics::flush_after_release();
    }
    return refs;
}
bool hookCompatibilityDevice(IUnknown* object,ID3D12Device* device) {
    if(!pinProxy())return false;
    auto& s=state();std::lock_guard lock(s.mutex);auto table=vtable(object);
    // Reject device/other-interface collisions BEFORE touching any slot.
    // Shared tables among compatibility instances reuse their own originals.
    if(object==reinterpret_cast<IUnknown*>(device) || table==vtable(device) ||
       s.devices.count(table) || s.objects.count(table) || s.lists.count(table) || s.queues.count(table))return false;
    auto found=s.compatibilityOwners.find(object);
    if(found!=s.compatibilityOwners.end() && found->second.device!=device)return false;
    if(found==s.compatibilityOwners.end()) {
        auto token=capture::new_lifetime_identity();if(!token)return false;
        s.compatibilityOwners.emplace(object,HookState::CompatibilityOwner{device,token});
    }
    auto& hooks=s.compatibility[table];
    const std::pair<int,void*> entries[]={
        {0,reinterpret_cast<void*>(&queryCompatibilityDevice)},
        {2,reinterpret_cast<void*>(&releaseCompatibilityDevice)},
        {3,reinterpret_cast<void*>(&createCompatibilityResource)},
        {4,reinterpret_cast<void*>(&createCompatibilityHeap)}};
    for(auto entry:entries)if(!patch(table,entry.first,entry.second,hooks))return false;
    return true;
}
// SDK 10.0.26100.0 um/ interface declaration lines (preceding MIDL supplies IID via __uuidof):
// d3d12sdklayers.h:3410/3876 InfoQueue/1, 1189/1076/1292 DebugDevice/1/2;
// d3d12.h:16391/16485 DRED/1, 16061/16166 DREDSettings/1,
// 29946 DeviceConfiguration, 28784/28876 Tools/1.
// Their own non-IUnknown methods expose diagnostics/configuration, CPU root
// serialization/deserialization and VA reservation, without tracked-object creation.
// LIMITATION: QueryInterface through a companion's own secondary vtable is
// unhooked and bypasses the fail-closed policy for ID3D12CompatibilityDevice,
// ID3D11On12Device/1/2 and unknown IIDs. Unknown/On12 queries are rejected on
// hooked device and compatibility tables, but this companion route can still
// return an unobserved compatibility table. Child-object GetDevice(riid) is another
// unhooked route that predates J-057. No companion vtable is mapped or patched.
// The accepted record's b equality bit lets a later authorized L2e check whether
// the companion shares the queried device vtable; it establishes no coverage
// for distinct vtables. Callback registration confers no tracked provenance.
unsigned companionDeviceInterface(REFIID iid) noexcept {
    if(iid==__uuidof(ID3D12InfoQueue))return 1;
    if(iid==__uuidof(ID3D12InfoQueue1))return 2;
    if(iid==__uuidof(ID3D12DebugDevice))return 3;
    if(iid==__uuidof(ID3D12DebugDevice1))return 4;
    if(iid==__uuidof(ID3D12DebugDevice2))return 5;
    if(iid==__uuidof(ID3D12DeviceRemovedExtendedData))return 6;
    if(iid==__uuidof(ID3D12DeviceRemovedExtendedData1))return 7;
    if(iid==__uuidof(ID3D12DeviceRemovedExtendedDataSettings))return 8;
    if(iid==__uuidof(ID3D12DeviceRemovedExtendedDataSettings1))return 9;
    if(iid==__uuidof(ID3D12DeviceConfiguration))return 10;
    if(iid==__uuidof(ID3D12Tools))return 11;
    if(iid==__uuidof(ID3D12Tools1))return 12;
    return 0;
}
// 34 process-lifetime reason literals: 17 device, 12 companion, 4 interop, unknown.
// The reason API retains pointers, so never pass it a formatted GUID buffer.
const char* unsupportedDeviceReason(REFIID iid) noexcept {
    if(iid==__uuidof(IUnknown))return "binding device interface unsupported: IUnknown";
    if(iid==__uuidof(ID3D12Object))return "binding device interface unsupported: ID3D12Object";
    if(iid==__uuidof(ID3D12Device))return "binding device interface unsupported: ID3D12Device";
    if(iid==__uuidof(ID3D12Device1))return "binding device interface unsupported: ID3D12Device1";
    if(iid==__uuidof(ID3D12Device2))return "binding device interface unsupported: ID3D12Device2";
    if(iid==__uuidof(ID3D12Device3))return "binding device interface unsupported: ID3D12Device3";
    if(iid==__uuidof(ID3D12Device4))return "binding device interface unsupported: ID3D12Device4";
    if(iid==__uuidof(ID3D12Device5))return "binding device interface unsupported: ID3D12Device5";
    if(iid==__uuidof(ID3D12Device6))return "binding device interface unsupported: ID3D12Device6";
    if(iid==__uuidof(ID3D12Device7))return "binding device interface unsupported: ID3D12Device7";
    if(iid==__uuidof(ID3D12Device8))return "binding device interface unsupported: ID3D12Device8";
    if(iid==__uuidof(ID3D12Device9))return "binding device interface unsupported: ID3D12Device9";
    if(iid==__uuidof(ID3D12Device10))return "binding device interface unsupported: ID3D12Device10";
    if(iid==__uuidof(ID3D12Device11))return "binding device interface unsupported: ID3D12Device11";
    if(iid==__uuidof(ID3D12Device12))return "binding device interface unsupported: ID3D12Device12";
    if(iid==__uuidof(ID3D12Device13))return "binding device interface unsupported: ID3D12Device13";
    if(iid==__uuidof(ID3D12Device14))return "binding device interface unsupported: ID3D12Device14";
    if(iid==__uuidof(ID3D12InfoQueue))return "binding device interface unsupported: ID3D12InfoQueue";
    if(iid==__uuidof(ID3D12InfoQueue1))return "binding device interface unsupported: ID3D12InfoQueue1";
    if(iid==__uuidof(ID3D12DebugDevice))return "binding device interface unsupported: ID3D12DebugDevice";
    if(iid==__uuidof(ID3D12DebugDevice1))return "binding device interface unsupported: ID3D12DebugDevice1";
    if(iid==__uuidof(ID3D12DebugDevice2))return "binding device interface unsupported: ID3D12DebugDevice2";
    if(iid==__uuidof(ID3D12DeviceRemovedExtendedData))return "binding device interface unsupported: ID3D12DeviceRemovedExtendedData";
    if(iid==__uuidof(ID3D12DeviceRemovedExtendedData1))return "binding device interface unsupported: ID3D12DeviceRemovedExtendedData1";
    if(iid==__uuidof(ID3D12DeviceRemovedExtendedDataSettings))return "binding device interface unsupported: ID3D12DeviceRemovedExtendedDataSettings";
    if(iid==__uuidof(ID3D12DeviceRemovedExtendedDataSettings1))return "binding device interface unsupported: ID3D12DeviceRemovedExtendedDataSettings1";
    if(iid==__uuidof(ID3D12DeviceConfiguration))return "binding device interface unsupported: ID3D12DeviceConfiguration";
    if(iid==__uuidof(ID3D12Tools))return "binding device interface unsupported: ID3D12Tools";
    if(iid==__uuidof(ID3D12Tools1))return "binding device interface unsupported: ID3D12Tools1";
    // d3d12compatibility.h:168; d3d11on12.h:182/301/418 (MIDL IID lines).
    // Compatibility retains this reason for canonical/table/installation gaps.
    // On12 wraps/acquires resources; v2 can unwrap an unobserved resource alias.
    if(iid==__uuidof(ID3D12CompatibilityDevice))return "binding device interface unsupported: ID3D12CompatibilityDevice";
    if(iid==__uuidof(ID3D11On12Device))return "binding device interface unsupported: ID3D11On12Device";
    if(iid==__uuidof(ID3D11On12Device1))return "binding device interface unsupported: ID3D11On12Device1";
    if(iid==__uuidof(ID3D11On12Device2))return "binding device interface unsupported: ID3D11On12Device2";
    return "binding device interface unsupported: unknown";
}
HRESULT queryObservedDevice(IUnknown* self,REFIID iid,void** out,bool compatibility) {
    using Fn=HRESULT(STDMETHODCALLTYPE*)(IUnknown*,REFIID,void**);
    HRESULT hr;
    try {hr=(compatibility?compatibilityOriginal<Fn>(self,0):creationOriginal<Fn>(self,0,true))(self,iid,out);}
    catch(...) {
        // COM must not throw. Preserve the exception after recording the gap;
        // no fabricated HRESULT, output mutation, or compensating Release.
        // observeNativeBinding, diagnostics::record and diagnostics::emit are
        // declared noexcept; diagnostics cannot replace the original exception.
        observeNativeBinding([&](auto& b){b.disable(unsupportedDeviceReason(iid));});
        char text[160]{};
        std::snprintf(text,sizeof text,"%s iid=%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            unsupportedDeviceReason(iid),iid.Data1,iid.Data2,iid.Data3,iid.Data4[0],iid.Data4[1],iid.Data4[2],iid.Data4[3],
            iid.Data4[4],iid.Data4[5],iid.Data4[6],iid.Data4[7]);
        diagnostics::emit(diagnostics::record(diagnostics::Event::device,0,0,0,text));
        throw;
    }
    // A native probe returning no interface publishes no alias to observe.
    // Preserve its exact HRESULT/output, including failure with non-null output.
    // Internal canonical-QI failures below still reject a returned interface.
    if(FAILED(hr) || !out || !*out)return hr;
    observeNativeBinding([&](auto& b){b.diagnostic_device_interface(
        iid==__uuidof(ID3D12Device4)?1u:iid==__uuidof(ID3D12Device8)?2u:iid==__uuidof(ID3D12Device10)?4u:0u);});
    bool supported=false;
    // Device extensions/companions retain identity-only mapping. Compatibility
    // alone installs its distinct secondary table; no object is wrapped.
    const auto companion=companionDeviceInterface(iid);
    const bool deviceInterface=iid==__uuidof(IUnknown) || iid==__uuidof(ID3D12Object) ||
        iid==__uuidof(ID3D12Device) || iid==__uuidof(ID3D12Device1) || iid==__uuidof(ID3D12Device2) ||
        iid==__uuidof(ID3D12Device3) || iid==__uuidof(ID3D12Device4) || iid==__uuidof(ID3D12Device5) ||
        iid==__uuidof(ID3D12Device6) || iid==__uuidof(ID3D12Device7) || iid==__uuidof(ID3D12Device8) ||
        iid==__uuidof(ID3D12Device9) || iid==__uuidof(ID3D12Device10) || iid==__uuidof(ID3D12Device11) ||
        iid==__uuidof(ID3D12Device12) || iid==__uuidof(ID3D12Device13) || iid==__uuidof(ID3D12Device14);
    const bool compat=iid==__uuidof(ID3D12CompatibilityDevice);
    ID3D12Device* discovered=nullptr;
    if(deviceInterface || companion || compat) {
        try {
            auto queryCanonical=[](IUnknown* object,IUnknown** canonical) {
                using Q=HRESULT(STDMETHODCALLTYPE*)(IUnknown*,REFIID,void**);
                Q query=nullptr;
                {auto& s=state();std::lock_guard lock(s.mutex);
                 auto table=vtable(object);auto found=s.devices.find(table);
                 auto c=s.compatibility.find(table);
                 query=reinterpret_cast<Q>(found!=s.devices.end() && found->second.originals[0]?
                     found->second.originals[0]:c!=s.compatibility.end() && c->second.originals[0]?
                     c->second.originals[0]:table[0]);}
                return query(object,__uuidof(IUnknown),reinterpret_cast<void**>(canonical));
            };
            if(compatibility)discovered=compatibilityDevice(self);
            else {auto& s=state();std::lock_guard lock(s.mutex);discovered=s.device?s.device:reinterpret_cast<ID3D12Device*>(self);}
            if(!discovered)throw std::runtime_error("missing compatibility owner");
            IUnknown *base=nullptr,*returned=nullptr;
            std::unique_ptr<IUnknown,ReleaseCOM> baseOwner,returnedOwner;
            auto baseHr=queryCanonical(discovered,&base);baseOwner.reset(base);
            auto returnedHr=queryCanonical(static_cast<IUnknown*>(*out),&returned);returnedOwner.reset(returned);
            supported=SUCCEEDED(baseHr) && SUCCEEDED(returnedHr) && base && base==returned;
            if(supported && compat)supported=hookCompatibilityDevice(static_cast<IUnknown*>(*out),discovered);
        } catch(...) {supported=false;}
    }
    // One static, reason-less device record per accepted QI (may repeat).
    // a is the bounded 1..12 SDK tag; b is companion/device vtable equality.
    // Only the equality bit is emitted, never either address.
    // No reason slot, wrapper, hook mapping or output/refcount change.
    if(supported && compat)
        diagnostics::emit(diagnostics::record(diagnostics::Event::device,13,0,0,"binding compatibility interface accepted"));
    if(supported && companion)
        diagnostics::emit(diagnostics::record(diagnostics::Event::device,companion,
            vtable(*out)==vtable(compatibility?static_cast<IUnknown*>(discovered):self),0,"binding companion interface accepted"));
    if(!supported)
        observeNativeBinding([&](auto& b){
            const auto* reason=unsupportedDeviceReason(iid);
            b.disable(reason);
            // Bounded copied record carries the opaque IID even when unknown;
            // aggregate reason cardinality stays fixed. No address is emitted.
            char text[160]{};
            std::snprintf(text,sizeof text,"%s iid=%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                reason,iid.Data1,iid.Data2,iid.Data3,iid.Data4[0],iid.Data4[1],iid.Data4[2],iid.Data4[3],
                iid.Data4[4],iid.Data4[5],iid.Data4[6],iid.Data4[7]);
            diagnostics::emit(diagnostics::record(diagnostics::Event::device,0,0,0,text));
        });
    return hr;
}
HRESULT STDMETHODCALLTYPE queryBindingDevice(ID3D12Device* self,REFIID iid,void** out) {
    HookAdmission admission;if(!admission)return E_FAIL;
    // J-xr-stereo-output: an XR-runtime QI inside a proxy XR call (possibly for a
    // runtime-private IID) is forwarded natively and never disables the observer.
    if(proxyCreationScopeActive()) {
        using Fn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,REFIID,void**);
        return creationOriginal<Fn>(self,0,true)(self,iid,out);
    }
    return queryObservedDevice(self,iid,out,false);
}
HRESULT STDMETHODCALLTYPE queryCompatibilityDevice(ID3D12CompatibilityDevice* self,REFIID iid,void** out) {
    HookAdmission admission;if(!admission)return E_FAIL;
    // J-compat-race: same rule as queryBindingDevice. A runtime QI on the compatibility
    // device inside a proxy XR call (e.g. for ID3D11On12Device) is forwarded natively
    // and never disables the observer; outside the scope the policy is unchanged.
    if(proxyCreationScopeActive()) {
        using Fn=decltype(&queryCompatibilityDevice);
        return compatibilityOriginal<Fn>(self,0)(self,iid,out);
    }
    return queryObservedDevice(self,iid,out,true);
}
ULONG STDMETHODCALLTYPE releaseBindingDevice(ID3D12Device* self) {
    HookAdmission admission;if(!admission)return 0;
    using Fn=ULONG(STDMETHODCALLTYPE*)(ID3D12Device*);
    // This was the sole source of the opaque `device` event kind. Neither
    // DXGI swap-chain creation nor CreateCommandQueue is intercepted here.
    // A non-final Release publishes no binding metadata. In particular overlay
    // hooks can create resources inside it: do not hold a cohort across native
    // Release or invent runtime-internal provenance for those ordinary events.
    // Cache the opaque key before the call; never dereference a dead device.
    const auto key=reinterpret_cast<uintptr_t>(self);
    auto refs=creationOriginal<Fn>(self,2,true)(self);
    if(refs&&proxyCreationScopeActive())return refs; // non-final runtime Release inside an XR call
    if(!refs) {
        BindingEvent event(key,true,binding::ObservationKind::device_release);
        // This observer has no per-device retirement record/token to erase:
        // final device Release permanently disables the whole observer, as
        // before. Resource/root/PSO cleanup retains its pre-call token checks.
        observeNativeBinding<diagnostics::DisableEvent::release>([](auto& b){b.disable("binding device final release");});
    }
    if(!refs) diagnostics::flush_after_release(); // Native Release returned; no mutex held.
    return refs;
}
template<class F> void observeBinding(ID3D12GraphicsCommandList* self,F&& fn) noexcept {
    perf::Scope cost(perf::setter); // FPS investigation (observe only)
    try { fn(); }
    catch(...) {
        auto& s=state();std::lock_guard lock(s.mutex);
        auto it=s.states.find(listKey_locked(self));if(it!=s.states.end()) {
            if(auto* log=diagnostics::sink();log&&log->ownership_pins())it->second.constantWitness.fail(diagnostics::RootConstantWitness::history);
            it->second.bindings.tracking_complete=false;
            if(!it->second.bindings.tracking_gap)it->second.bindings.tracking_gap="rejected:setter observation exception/allocation failure";}
    }
}
void STDMETHODCALLTYPE setRoot(ID3D12GraphicsCommandList* self,ID3D12RootSignature* root) {
    HookAdmission admission;if(!admission)return;
    ConstantCall constantCall(self);
    using Fn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12RootSignature*);
    original<Fn>(self,30)(self,root);
    constantCall.returned();
    observeBinding(self,[&] {
        auto next=retain(root);std::shared_ptr<ID3D12RootSignature> old;
        auto& s=state();
        {std::lock_guard lock(s.mutex);auto& current=s.states[listKey_locked(self)];old.swap(current.root);current.root=std::move(next);
         auto k=s.rootKeys.find(root);
         auto key=k==s.rootKeys.end()?root:k->second;
         auto found=s.roots.find(key);std::optional<metadata::RootBindingMetadata> layout;
         if(found!=s.roots.end()) layout=found->second;
         auto identity=s.rootIdentities.find(key);
         const auto token=identity==s.rootIdentities.end()?0:identity->second;
         const bool changed=current.bindings.root_id!=reinterpret_cast<uintptr_t>(key)||current.bindings.root_identity!=token;
         current.bindings.set_root(reinterpret_cast<uintptr_t>(key),std::move(layout),
             token);
         if(auto* l=l3_list_locked(current);l&&changed) {
             l->t10_parameter=UINT32_MAX;l->sources[0]={};l->table_heap=nullptr;l->table_slot=nullptr;
             l->t10_vs_parameter=UINT32_MAX;l->t10_vs_table=false;l->vs_source={};
             if(current.bindings.root)l3ConfigureT10(*l,*current.bindings.root);
         }
         configureConstantWitness(current);}
    });
}
void STDMETHODCALLTYPE setHeaps(ID3D12GraphicsCommandList* self,UINT count,ID3D12DescriptorHeap* const* heaps) {
    HookAdmission admission;if(!admission)return;
    ConstantCall constantCall(self);
    using Fn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,ID3D12DescriptorHeap* const*);
    original<Fn>(self,28)(self,count,heaps);
    constantCall.returned();
    observeBinding(self,[&] {
        if(count>2 || (count && !heaps)) throw std::runtime_error("unsupported heaps");
        std::vector<std::shared_ptr<ID3D12DescriptorHeap>> next,old;std::vector<capture::Id> ids;
        for(UINT i=0;i<count;++i) {next.push_back(retain(heaps[i]));ids.push_back(reinterpret_cast<uintptr_t>(heaps[i]));}
        auto& s=state();
        {std::lock_guard lock(s.mutex);auto& current=s.states[listKey_locked(self)];old.swap(current.heaps);current.heaps=std::move(next);
         if(auto* l=l3_list_locked(current))if(current.bindings.heaps!=ids){l->table_heap=nullptr;l->table_slot=nullptr;l->sources[0]={};}
         if(constantCall.enabled&&current.bindings.heaps!=ids){current.tableWitness.written=false;current.samplerWitness.written=false;}
         current.bindings.set_heaps(std::move(ids));}
    });
}
void STDMETHODCALLTYPE setTable(ID3D12GraphicsCommandList* self,UINT index,D3D12_GPU_DESCRIPTOR_HANDLE handle) {
    HookAdmission admission;if(!admission)return;
    ConstantCall constantCall(self);
    using Fn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,D3D12_GPU_DESCRIPTOR_HANDLE);
    original<Fn>(self,32)(self,index,handle);
    constantCall.returned();
    observeBinding(self,[&]{auto& s=state();std::lock_guard lock(s.mutex);
        auto& l=s.states[listKey_locked(self)];l.bindings.set_value(index,metadata::ParameterKind::table,handle.ptr);
        if(auto* observation=l3_list_locked(l);observation&&observation->t10_table&&observation->t10_parameter==index) {
            observation->table_heap=nullptr;observation->table_slot=nullptr;observation->sources[0]={};
            if(const auto* d=s.nativeBindings.diagnostic())for(auto heap:l.bindings.heaps) {
                const auto loc=d->bound(heap,handle.ptr,l.native_binding_device);
                if(loc.flags||loc.type!=0||observation->t10_offset>UINT32_MAX-loc.slot)continue;
                const auto h=d->heaps.find(loc.key);const auto v=d->slots.find({loc.serial,loc.slot+observation->t10_offset});
                if(h==d->heaps.end()||v==d->slots.end())continue;
                if(observation->table_heap){observation->table_heap=nullptr;observation->table_slot=nullptr;observation->gap=true;break;}
                observation->table_heap=&h->second;observation->table_slot=&v->second;observation->table_epoch=loc.serial;observation->table_diagnostics=d;
                observation->table_identity=s.nativeBindings.diagnostic_identity();
            }
        }
        for(auto* witness:{&l.tableWitness,&l.samplerWitness})if(constantCall.enabled&&index==witness->parameter) {
            using D=diagnostics::DescriptorTableDiagnostics;
            D::Location location;location.flags=D::unknown_base;
            bool found=false;
            if(const auto* d=s.nativeBindings.diagnostic())for(auto heap:l.bindings.heaps) {
                auto value=d->bound(heap,handle.ptr,l.native_binding_device);
                if(value.flags)continue;
                if(found){location={};location.flags=D::unknown_base;break;}
                found=true;location=value;
            }
            const auto* d=s.nativeBindings.diagnostic();
            witness->write(constantCall.call,l.constantWitness,index,handle.ptr,location,
                constantCall.descriptorBefore,d&&!s.bindingEvents.load()?d->write_ordinal:UINT32_MAX);
        }
    });
}
template<int Slot,metadata::ParameterKind Kind>
void STDMETHODCALLTYPE setAddress(ID3D12GraphicsCommandList* self,UINT index,D3D12_GPU_VIRTUAL_ADDRESS address) {
    HookAdmission admission;if(!admission)return;
    ConstantCall constantCall(self);
    using Fn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,D3D12_GPU_VIRTUAL_ADDRESS);
    original<Fn>(self,Slot)(self,index,address);
    constantCall.returned();
    observeBinding(self,[&]{auto& s=state();std::lock_guard lock(s.mutex);auto& list=s.states[listKey_locked(self)];list.bindings.set_value(index,Kind,address);
        if(auto* l=l3_list_locked(list);l&&Kind==metadata::ParameterKind::srv&&index==l->t10_parameter) {
            l->sources[0]={s.nativeBindings.diagnostic_address(address,416,list.native_binding_device),address,416};l->t10_table=false;
        }
        // J-180: the vertex-stage binding is tracked on its own (same record; a different parameter in a split root). A table candidate is not tracked.
        if(auto* l=l3_list_locked(list);l&&Kind==metadata::ParameterKind::srv&&index==l->t10_vs_parameter&&!l->t10_vs_table) {
            l->vs_source={s.nativeBindings.diagnostic_address(address,416,list.native_binding_device),address,416};
        }});
}
void STDMETHODCALLTYPE setConstants(ID3D12GraphicsCommandList* self,UINT index,UINT count,const void* data,UINT offset) {
    HookAdmission admission;if(!admission)return;
    ConstantCall constantCall(self);
    using Fn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,UINT,const void*,UINT);
    original<Fn>(self,36)(self,index,count,data,offset);
    constantCall.returned();
    observeBinding(self,[&]{auto& s=state();std::lock_guard lock(s.mutex);s.states[listKey_locked(self)].bindings.set_constants(index,offset,static_cast<const uint32_t*>(data),count);
        if(constantCall.enabled) {auto& l=s.states.at(listKey_locked(self));l.constantWitness.write(constantCall.call,l.bindings.list_identity,l.bindings.generation,index,offset,static_cast<const uint32_t*>(data),count);}
        if(auto* log=diagnostics::sink();log&&log->ownership_pins()&&data&&count&&index<64&&offset<=64&&count<=64-offset&&s.constantWordCalls<65535)++s.constantWordCalls;});
}
void STDMETHODCALLTYPE setConstant(ID3D12GraphicsCommandList* self,UINT index,UINT value,UINT offset) {
    HookAdmission admission;if(!admission)return;
    ConstantCall constantCall(self);
    using Fn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,UINT,UINT);
    original<Fn>(self,34)(self,index,value,offset);
    constantCall.returned();
    observeBinding(self,[&]{auto& s=state();std::lock_guard lock(s.mutex);s.states[listKey_locked(self)].bindings.set_constants(index,offset,&value,1);
        if(constantCall.enabled) {auto& l=s.states.at(listKey_locked(self));l.constantWitness.write(constantCall.call,l.bindings.list_identity,l.bindings.generation,index,offset,&value,1);}
        if(auto* log=diagnostics::sink();log&&log->ownership_pins()&&index<64&&offset<64&&s.constantWordCalls<65535)++s.constantWordCalls;});
}
void STDMETHODCALLTYPE setIndex(ID3D12GraphicsCommandList* self,const D3D12_INDEX_BUFFER_VIEW* view) {
    HookAdmission admission;if(!admission)return;
    ConstantCall constantCall(self);
    using Fn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,const D3D12_INDEX_BUFFER_VIEW*);
    original<Fn>(self,43)(self,view);
    constantCall.returned();
    observeBinding(self,[&]{auto& s=state();std::lock_guard lock(s.mutex);auto& b=s.states[listKey_locked(self)].bindings;++b.revision;b.index.reset();
        auto& list=s.states.at(listKey_locked(self));if(auto* l=l3_list_locked(list)) {
            l->sources[33]={};if(view)l->sources[33]={s.nativeBindings.diagnostic_address(view->BufferLocation,view->SizeInBytes,list.native_binding_device),view->BufferLocation,view->SizeInBytes};
        }
        if(view) b.index=capture::BufferView{view->BufferLocation,0,view->SizeInBytes,view->Format==DXGI_FORMAT_R16_UINT?2u:view->Format==DXGI_FORMAT_R32_UINT?4u:0u};});
}
void STDMETHODCALLTYPE setVertices(ID3D12GraphicsCommandList* self,UINT start,UINT count,const D3D12_VERTEX_BUFFER_VIEW* views) {
    HookAdmission admission;if(!admission)return;
    ConstantCall constantCall(self);
    using Fn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,UINT,const D3D12_VERTEX_BUFFER_VIEW*);
    original<Fn>(self,44)(self,start,count,views);
    constantCall.returned();
    observeBinding(self,[&]{auto& s=state();std::lock_guard lock(s.mutex);auto& b=s.states[listKey_locked(self)].bindings;++b.revision;
        if(start>32 || count>32-start) {b.tracking_complete=false;
            if(!b.tracking_gap)b.tracking_gap="rejected:IA vertex buffer slot out of range";return;}
        for(UINT i=0;i<count;++i) {
            if(views) b.vertices[start+i]={views[i].BufferLocation,0,views[i].SizeInBytes,views[i].StrideInBytes};
            else b.vertices.erase(start+i);
            auto& list=s.states.at(listKey_locked(self));if(auto* l=l3_list_locked(list)) {
                l->sources[start+i+1]={};if(views)l->sources[start+i+1]={s.nativeBindings.diagnostic_address(views[i].BufferLocation,views[i].SizeInBytes,list.native_binding_device),views[i].BufferLocation,views[i].SizeInBytes};
            }
        }
    });
}
void STDMETHODCALLTYPE executeBundle(ID3D12GraphicsCommandList* self,ID3D12GraphicsCommandList* bundle) {
    HookAdmission admission;if(!admission)return;
    CopyNativeCall copyObservation(self);
    ConstantCall constantCall(self);
    if(constantCall.enabled) {auto& s=state();std::lock_guard lock(s.mutex);
        auto it=s.states.find(listKey_locked(self));if(it!=s.states.end())it->second.constantWitness.fail(diagnostics::RootConstantWitness::bundle);}
    using Fn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12GraphicsCommandList*);
    copyObservation.event(gpu_copy::Event::bundle);
    original<Fn>(self,27)(self,bundle);
    constantCall.returned();
    observeBinding(self,[&]{auto& s=state();std::lock_guard lock(s.mutex);auto& b=s.states[listKey_locked(self)].bindings;++b.revision;b.tracking_complete=false;
        if(!b.tracking_gap)b.tracking_gap="rejected:bundle use";});
}
void STDMETHODCALLTYPE executeIndirect(ID3D12GraphicsCommandList* self,ID3D12CommandSignature* signature,UINT count,
                                      ID3D12Resource* arguments,UINT64 offset,ID3D12Resource* counter,UINT64 counterOffset) {
    HookAdmission admission;if(!admission)return;
    CopyNativeCall copyObservation(self);
    ConstantCall constantCall(self);
    if(constantCall.enabled) {auto& s=state();std::lock_guard lock(s.mutex);
        auto it=s.states.find(listKey_locked(self));if(it!=s.states.end())it->second.constantWitness.fail(diagnostics::RootConstantWitness::indirect);}
    using Fn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12CommandSignature*,UINT,ID3D12Resource*,UINT64,ID3D12Resource*,UINT64);
    copyObservation.event(gpu_copy::Event::indirect);
    fxAtlasComputeRecorded(self,true); // review 2026-11-01 F2: read-only census (an indirect call may dispatch)
    original<Fn>(self,59)(self,signature,count,arguments,offset,counter,counterOffset);
    constantCall.returned();
    observeBinding(self,[&]{auto& s=state();std::lock_guard lock(s.mutex);auto& b=s.states[listKey_locked(self)].bindings;++b.revision;b.tracking_complete=false;
        if(!b.tracking_gap)b.tracking_gap="rejected:ExecuteIndirect use";});
}
} // namespace
bool hookCaptureDevice(ID3D12Device* device) {
    HookAdmission admission;if(!admission)return false;
    if(!device || !pinProxy()) return false;
    bool installed=false;
    // Step-1 only (default OFF: no QI, no slot 51): decide outside every lock
    // whether CreateCommandList1 lives in this same base table.
    const auto list1=g_step1.load()?step1Device4SharesTable(device):Step1List1Hook::not_attempted;
    auto list1Outcome=list1;
    // J-3b4 (early owner only, default OFF): queue creation witnesses on the
    // owner's own device. Same shared-table rule for CreateCommandQueue1.
    auto* earlyOwner=g_step1.load();
    const bool earlyQueues=earlyOwner&&earlyOwner->early&&device==earlyOwner->device;
    const auto queue1=earlyQueues?step1Device9SharesTable(device):Step1List1Hook::not_attempted;
    auto queue1Outcome=queue1;auto queueOutcome=Step1List1Hook::not_attempted;
    {auto& s=state();std::lock_guard lock(s.mutex);auto table=vtable(device);if(s.compatibility.count(table))return false;auto& hooks=s.devices[table];
     installed=patch(table,16,reinterpret_cast<void*>(&createRoot),hooks) &&
       patch(table,10,reinterpret_cast<void*>(&createGraphicsPSO),hooks) &&
       patch(table,12,reinterpret_cast<void*>(&createList),hooks);
     // Default OFF: slot 9 is untouched unless the step-1 experiment owner exists.
     // A failed patch only leaves allocators unwitnessed (every Reset refuses).
     if(installed&&g_step1.load())(void)patch(table,9,reinterpret_cast<void*>(&step1CreateAllocator),hooks);
     // J-3b3: same rule for CreateCommandList1 (slot 51), shared table only.
     if(installed&&list1==Step1List1Hook::installed&&
        !patch(table,51,reinterpret_cast<void*>(&step1CreateList1),hooks))list1Outcome=Step1List1Hook::patch_failed;
     // Re-running this at swap-chain discovery finds the live replacement and the
     // retained original: patch() is idempotent, nothing is hooked twice.
     if(installed&&earlyQueues)
         queueOutcome=patch(table,8,reinterpret_cast<void*>(&step1CreateQueue),hooks)?Step1List1Hook::installed:Step1List1Hook::patch_failed;
     if(installed&&queue1==Step1List1Hook::installed&&
        !patch(table,75,reinterpret_cast<void*>(&step1CreateQueue1),hooks))queue1Outcome=Step1List1Hook::patch_failed;}
    if(list1!=Step1List1Hook::not_attempted)step1NoteList1Hook(installed?list1Outcome:Step1List1Hook::patch_failed);
    if(earlyQueues) {
        step1NoteHook(earlyOwner->queueHook,installed?queueOutcome:Step1List1Hook::patch_failed);
        if(queue1!=Step1List1Hook::not_attempted)step1NoteHook(earlyOwner->queue1Hook,installed?queue1Outcome:Step1List1Hook::patch_failed);
    }
    // Extension device tables stay untouched (J-056/D-86). Closed-created
    // lists are discovered at their first Reset on an already-patched list table.
    if(!installed){auto& s=state();std::lock_guard lock(s.mutex);s.nativeBindings.disable();
        diagnostics::observer_disabled("capture device hook incomplete");}
    return installed;
}
bool hookNativeBindingDevice(ID3D12Device* device) {
    HookAdmission admission;if(!admission)return false;
    if(!hookCaptureDevice(device)) return false;
    auto& s=state();std::lock_guard lock(s.mutex);auto table=vtable(device);if(s.compatibility.count(table))return false;auto& hooks=s.devices[table];
    const std::pair<int,void*> bindingHooks[]={
        {0,reinterpret_cast<void*>(&queryBindingDevice)}, {2,reinterpret_cast<void*>(&releaseBindingDevice)},
        {14,reinterpret_cast<void*>(&createBindingHeap)}, {17,reinterpret_cast<void*>(&writeBindingCbv)},
        {18,reinterpret_cast<void*>(&writeBindingSrv)}, {19,reinterpret_cast<void*>(&writeBindingUav)},
        {20,reinterpret_cast<void*>(&unsupportedView<20,D3D12_RENDER_TARGET_VIEW_DESC>)},
        {21,reinterpret_cast<void*>(&unsupportedView<21,D3D12_DEPTH_STENCIL_VIEW_DESC>)},
        {22,reinterpret_cast<void*>(&unsupportedSampler)}, {23,reinterpret_cast<void*>(&copyBindingDescriptors)},
        {24,reinterpret_cast<void*>(&copyBindingSimple)}, {27,reinterpret_cast<void*>(&createBindingResource)},
        {29,reinterpret_cast<void*>(&unsupportedPlaced)}, {30,reinterpret_cast<void*>(&unsupportedReserved)}};
    for(auto entry:bindingHooks) if(!patch(table,entry.first,entry.second,hooks)) {s.nativeBindings.disable();
        diagnostics::observer_disabled("binding device hook incomplete");return false;}
    return true;
}
// J-3b4: device-created notification from the optional d3d12.dll proxy
// (AczVrNotifyDeviceCreated). Called on the game's thread right after the real
// D3D12CreateDevice returned success, never under loader lock and never with a
// proxy lock held. Default OFF: unless step-1 and gpu_copy_early_device_hook are
// latched it returns without any QI, patch, allocation or record. When ON it runs
// the capture half of discovery's device hook install early (hookCaptureDevice:
// root/PSO/createList, CreateCommandAllocator, CreateCommandList1, plus
// CreateCommandQueue/1 witnesses) so creations before the first swap chain are
// witnessed; discovery later joins the queue, re-runs that idempotent install
// and only then adds the native binding observer's device hooks (J-3b5).
// Return: 0 off/ignored, 1 owner created and hooks installed, 2 no ID3D12Device,
// 3 repeated/other device (owner exists), 4 owner unavailable, 5 hook incomplete.
unsigned notifyEarlyDevice(IUnknown* created,const GUID* iid) noexcept {
    try {
        if(!created||!iid)return 0;
#ifndef ACZ_VR_OFFLINE_TEST
        if(!launch_mode::vr_enabled())return 0; // LAUNCH-MODE: VR Off = the d3d12.dll proxy's notification does nothing
#endif
        // The first factory call normally activates diagnostics; the device can
        // come first. Activation is outside loader lock here (ordinary call).
        if(!g_loading)diagnostics::activate();
        if(!step1EarlyEnabled())return 0;
        HookAdmission admission;if(!admission)return 0;
        static std::atomic<unsigned> calls{0};
        const unsigned call=calls.fetch_add(1)+1;
        char text[diagnostics::text_capacity]{};
        auto line=[&](const char* outcome,ID3D12Device* device,unsigned code) {
            if(call>4)return code; // bounded: at most four early-device lines
            std::snprintf(text,sizeof text,"gpu-copy-step1;event=early-device;outcome=%s;call=%u;device=%llx;iid=%08lx;"
                "cl1-hook=%u;cq-hook=%u;cq1-hook=%u;witness-start=early-device",outcome,call,
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(device)),iid->Data1,
                g_step1.load()?g_step1.load()->list1Hook.load():0u,g_step1.load()?g_step1.load()->queueHook.load():0u,
                g_step1.load()?g_step1.load()->queue1Hook.load():0u);
            step1EmitStartup(text);return code;
        };
        ID3D12Device* device=nullptr;
        if(FAILED(created->QueryInterface(IID_PPV_ARGS(&device)))||!device)return line("no-device-interface",nullptr,2);
        std::unique_ptr<ID3D12Device,ReleaseCOM> owned(device);
        if(auto* st=g_step1.load()) {
            // Owner already exists (an earlier device or discovery first).
            if(st->early&&st->device==device) {
                (void)st->registry.device_created(true,step1Route(device)); // live route: token kept
                return line("repeated",device,3);
            }
            return line(st->early?"other-device-ignored":"after-discovery-ignored",device,3);
        }
        {auto& s=state();std::lock_guard lock(s.mutex);if(s.device)return line("after-discovery-ignored",device,3);}
        auto* st=step1WitnessEarlyDevice(device);
        if(!st)return line("owner-unavailable",device,4);
        // Retained for the process lifetime: the owner's device route identity
        // (HookState retains the discovered device the same way). Never released.
        owned.release();
        // J-3b5: creation witnesses only. The native binding observer's device
        // hooks (QueryInterface/Release/heap/descriptor/resource slots) are still
        // installed by swap-chain discovery (hookNativeBindingDevice there), as in
        // every run without this hook. Installing them here exposed the device
        // QI to DXGI's own swap-chain creation, which queries two runtime-private
        // IIDs (de18ef3a-..., ca4f6eec-..., present only in dxgi.dll/d3d12core.dll)
        // and disabled the observer ("binding device interface unsupported:
        // unknown", live run 4 evidence 63/66/64). The fail-closed QI policy is
        // unchanged after discovery.
        const bool hooked=hookCaptureDevice(device);
        return line(hooked?"hooked":"hook-incomplete",device,hooked?1:5);
    } catch(...) {return 0;}
}
namespace {
// Called under capture mutex. Resolve from the actual live State, never from
// provider-modified fields of an otherwise matching historical state copy.
const ListState* liveBindingState(const capture::State& supplied) {
    auto& s=state();if(s.bindingEvents.load()!=0 || !supplied.list_identity) return nullptr;
    for(const auto& list:s.states) {
        const auto& b=list.second.bindings;
        if(b.list_identity==supplied.list_identity&&!listPointFailure_locked(list.second,supplied)&&b.tracking_complete)
            return &list.second;
    }
    return nullptr;
}
binding::NativeResolution noBindingPoint(const capture::State* supplied=nullptr) {
    binding::NativeResolution r;r.rejection="missing/stale/busy native capture point";
    if(supplied) {
        r.rejection="rejected:no list identity";
        for(const auto& item:state().states)if(item.second.bindings.list_identity==supplied->list_identity) {
            if(auto reason=listPointFailure_locked(item.second,*supplied))r.rejection=reason;
            else r.rejection="missing/stale/busy native capture point";
            break;
        }
    }
    return r;
}
binding::NativeResolution resolveNativeTableBinding_locked(const capture::State& supplied,uint32_t parameter,capture::Id heap,uint32_t offset) {
    auto& s=state();
    auto live=liveBindingState(supplied);if(!live) return noBindingPoint(&supplied);
    try {
        auto r=s.nativeBindings.table(live->bindings,parameter,heap,offset,live->native_binding_device);
        return s.bindingEvents.load()?noBindingPoint():r;
    } catch(...) {s.nativeBindings.disable();diagnostics::observer_disabled("native table resolution exception");return noBindingPoint();}
}
binding::NativeResolution resolveNativeRootBinding_locked(const capture::State& supplied,uint32_t parameter,uint32_t bytes,uint32_t elements,uint32_t stride) {
    auto& s=state();
    auto live=liveBindingState(supplied);if(!live) return noBindingPoint(&supplied);
    try {
        auto r=s.nativeBindings.root(live->bindings,parameter,bytes,elements,stride,live->native_binding_device);
        return s.bindingEvents.load()?noBindingPoint():r;
    } catch(...) {s.nativeBindings.disable();diagnostics::observer_disabled("native root resolution exception");return noBindingPoint();}
}
binding::Status currentNativeBinding_locked(const binding::NativeResolution& value,const capture::State& supplied) {
    auto& s=state();
    auto live=liveBindingState(supplied);if(!live) return {"missing/stale/busy native capture point"};
    try {
        auto r=s.nativeBindings.current(value,live->bindings,live->native_binding_device);
        return s.bindingEvents.load()?binding::Status{"busy native capture point"}:r;
    } catch(...) {s.nativeBindings.disable();diagnostics::observer_disabled("native currentness exception");return {"native metadata observation failed"};}
}
} // namespace
binding::NativeResolution resolveNativeTableBinding(const capture::State& supplied,uint32_t parameter,capture::Id heap,uint32_t offset) {
    auto& s=state();std::lock_guard lock(s.mutex);return resolveNativeTableBinding_locked(supplied,parameter,heap,offset);
}
binding::NativeResolution resolveNativeRootBinding(const capture::State& supplied,uint32_t parameter,uint32_t bytes,uint32_t elements,uint32_t stride) {
    auto& s=state();std::lock_guard lock(s.mutex);return resolveNativeRootBinding_locked(supplied,parameter,bytes,elements,stride);
}
binding::Status currentNativeBinding(const binding::NativeResolution& value,const capture::State& supplied) {
    auto& s=state();std::lock_guard lock(s.mutex);return currentNativeBinding_locked(value,supplied);
}
namespace native_evidence {
namespace {
// Recursion is a same-thread property. Concurrent callers serialize on the
// existing capture mutex; a nested call poisons its outer attempt as well.
thread_local bool exportActive=false;
thread_local bool exportReentered=false;
Export refused(const char* why) noexcept { Export r;r.reason=why;return r; }
bool same_selection(const Selection& a,const Selection& b) noexcept {
    return a.category==b.category && a.root==b.root && a.parameter==b.parameter && a.offset==b.offset &&
        a.count==b.count && a.resource==b.resource && a.heap==b.heap &&
        a.slot==b.slot && a.revision==b.revision && a.address==b.address &&
        a.bytes==b.bytes && a.elements==b.elements && a.stride==b.stride;
}
bool table_range_owned(const metadata::RootParameter& p,uint32_t offset,metadata::BindingKind kind) noexcept {
    if(p.kind!=metadata::ParameterKind::table)return false;
    uint64_t next=0;unsigned matches=0;
    for(const auto& range:p.ranges) {
        const uint64_t start=range.offset==metadata::append_range?next:range.offset;
        const uint64_t end=start+range.count;
        if(end>UINT32_MAX+uint64_t(1))return false;
        if(offset>=start && offset<end) {
            if(range.kind!=kind)return false;
            ++matches;
        }
        next=end;
    }
    return matches==1;
}
} // namespace
Export export_metadata(const ExpectedPoint& expected,const Selection* selections,size_t count,Limits limits) noexcept {
    HookAdmission admission;if(!admission)return refused("ownership-shutdown");
    if(exportActive) {
        exportReentered=true;
        return refused("BusyReentry");
    }
    exportActive=true;
    exportReentered=false;
    Export candidate;
    const char* failure=nullptr;
    try {
        if(!expected.list || !expected.generation || !expected.device || !expected.root || !expected.pso)
            failure="MissingExpectedPoint";
        else if((count && !selections) || !count || count>max_selections || count>limits.selections ||
                !limits.selections || !limits.resources || !limits.words ||
                limits.selections>max_selections || limits.resources>max_resources || limits.words>max_root_words)
            failure="SelectionBounds";
        // Own the bounded request before the capture lock. Nothrow allocation
        // makes failure observable even under an allocation-faulting caller.
        std::unique_ptr<Selection[]> owned;
        if(!failure) {
            owned.reset(new(std::nothrow) Selection[count]);
            if(!owned)failure="AllocationOrObservationFailure";
            else std::copy_n(selections,count,owned.get());
        }
        if(!failure) {
            auto& s=state();std::lock_guard lock(s.mutex); // One acquisition for validation, copy, and final recheck.
            if(!s.nativeBindings.enabled())failure="ObserverDisabled";
            else if(s.bindingEvents.load() || s.rootCreates || s.metadataReleases)failure="BusyNativeObservation";
            const ListState* list=nullptr;
            if(!failure) {
                for(const auto& pair:s.states)if(pair.second.bindings.list_identity==expected.list) {
                    list=&pair.second;break;
                }
                if(!list)failure="StaleOrMissingPoint";
                else {
                    const auto& b=list->bindings;
                    if(b.generation!=expected.generation || b.revision!=expected.revision ||
                       list->native_binding_device!=expected.device)failure="PointMismatch";
                    else if(!list->capture_hooks || !list->interfaces_observed || !list->cleanup_established ||
                            !list->reset_observed ||
                            !list->native_binding_history || list->observation_gap ||
                            b.tracking_gap || !b.tracking_complete || !b.list_identity)
                        failure="RecordingGap";
                    else if(!b.compatible_root() || b.root_identity!=expected.root ||
                            b.pipeline.identity!=expected.pso)failure="RootPsoMismatch";
                }
            }
            if(!failure) {
                const auto& b=list->bindings;
                const auto& parameters=b.root->layout().parameters;
                candidate.recording=expected;
                candidate.recording_origin=Provenance::ObservedMetadata;
                candidate.coverage=Coverage::CreationOrResetBoundaryObserved;
                for(size_t i=0;i<count && !failure;++i) {
                    const auto& pick=owned[i];
                    if(!pick.root || pick.root!=expected.root) {
                        failure="RootStampMismatch";break;
                    }
                    if(pick.category==Category::IA || pick.category==Category::Texture ||
                       pick.category==Category::Sampler || pick.category==Category::OpaqueWritable) {
                        failure="UnsupportedCategory";break;
                    }
                    if(pick.category!=Category::TableBuffer && pick.category!=Category::RootCbv &&
                       pick.category!=Category::RootSrv && pick.category!=Category::RootConstants) {
                        failure="UnsupportedCategory";break;
                    }
                    for(size_t j=0;j<i;++j)if(same_selection(pick,owned[j])) {
                        failure="DuplicateSelection";break;
                    }
                    if(failure)break;
                    if(pick.parameter>=parameters.size() || pick.parameter>=64) {
                        failure="UnsupportedSelection";break;
                    }
                    const auto& p=parameters[pick.parameter];
                    Entry entry;entry.category=pick.category;entry.parameter=pick.parameter;entry.offset=pick.offset;
                    if(pick.category==Category::RootConstants) {
                        if(p.kind!=metadata::ParameterKind::constants || !pick.count || pick.count>limits.words ||
                           pick.offset>=p.constant_count || pick.count>p.constant_count-pick.offset ||
                           pick.offset>max_root_words || pick.count>max_root_words-pick.offset ||
                           pick.resource.id || pick.resource.generation || pick.heap.id ||
                           pick.heap.generation || pick.slot || pick.revision || pick.address ||
                           pick.bytes || pick.elements || pick.stride) {
                            failure="RootRangeInvalid";break;
                        }
                        auto found=b.roots.find(pick.parameter);
                        if(found==b.roots.end() || found->second.kind!=metadata::ParameterKind::constants ||
                           found->second.constants.size()<size_t(pick.offset)+pick.count ||
                           found->second.written.size()<size_t(pick.offset)+pick.count) {
                            failure="RootWordsUnwritten";break;
                        }
                        for(uint32_t word=pick.offset;word<pick.offset+pick.count;++word)
                            if(!found->second.written[word]) {failure="RootWordsUnwritten";break;}
                        if(failure)break;
                        entry.word_count=pick.count;
                        std::copy_n(found->second.constants.begin()+pick.offset,pick.count,entry.words.begin());
                        entry.binding_origin=entry.words_origin=Provenance::ObservedMetadata;
                    } else {
                        binding::NativeResolution resolved;
                        if(pick.category==Category::TableBuffer) {
                            if(p.kind!=metadata::ParameterKind::table || pick.count!=1 ||
                               !pick.heap.id || !pick.heap.generation || !pick.revision ||
                               pick.address || pick.bytes || pick.elements || pick.stride) {
                                failure="UnsupportedSelection";break;
                            }
                            resolved=s.nativeBindings.table(b,pick.parameter,pick.heap.id,pick.offset,
                                                            list->native_binding_device);
                        } else {
                            const bool cbv=pick.category==Category::RootCbv;
                            if(p.kind!=(cbv?metadata::ParameterKind::cbv:metadata::ParameterKind::srv) ||
                               pick.count || pick.offset || !pick.address || pick.heap.id ||
                               pick.heap.generation || pick.slot || pick.revision ||
                               (cbv?(!pick.bytes || pick.elements || pick.stride):
                                    (pick.bytes || !pick.elements || !pick.stride ||
                                     uint64_t(pick.elements)*pick.stride>UINT32_MAX))) {
                                failure="ProviderLayoutInvalid";break;
                            }
                            resolved=s.nativeBindings.root(b,pick.parameter,pick.bytes,pick.elements,
                                                           pick.stride,list->native_binding_device);
                        }
                        if(!resolved || !s.nativeBindings.current(resolved,b,list->native_binding_device)) {
                            failure="StaleOrUnsupportedBinding";break;
                        }
                        const auto& v=*resolved.snapshot;
                        if(!v.resource.id || !v.resource.generation || v.resource!=pick.resource ||
                           !v.resource_bytes || !v.descriptor.buffer ||
                           (v.descriptor.kind!=metadata::BindingKind::cbv &&
                            v.descriptor.kind!=metadata::BindingKind::srv)) {
                            failure="ResourceStampMismatch";break;
                        }
                        if(pick.category==Category::TableBuffer) {
                            if(v.heap!=pick.heap || v.slot!=pick.slot || v.revision!=pick.revision ||
                               !table_range_owned(p,pick.offset,v.descriptor.kind)) {
                                failure="DestinationStampMismatch";break;
                            }
                            entry.heap=v.heap;entry.slot=v.slot;entry.revision=v.revision;
                            entry.source_heap=v.source_heap;entry.source_slot=v.source_slot;
                            entry.source_revision=v.source_revision;
                            entry.destination_origin=entry.descriptor_origin=Provenance::ObservedMetadata;
                            if(v.source_heap.id)entry.source_origin=Provenance::ObservedMetadata;
                        } else {
                            auto root=b.roots.find(pick.parameter);
                            if(root==b.roots.end() || root->second.value!=pick.address ||
                               v.descriptor.view.size!=(pick.category==Category::RootCbv?
                                   uint64_t(pick.bytes):uint64_t(pick.elements)*pick.stride)) {
                                failure="ProviderSpanMismatch";break;
                            }
                            entry.address=pick.address;entry.bytes=pick.bytes;
                            entry.elements=pick.elements;entry.stride=pick.stride;
                            entry.layout_origin=Provenance::ProviderDeclared;
                            entry.descriptor_origin=Provenance::ProviderDeclared;
                            entry.containment_origin=Provenance::ObservedMetadata;
                        }
                        entry.resource=v.resource;entry.descriptor=v.descriptor;
                        entry.resource_bytes=v.resource_bytes;entry.device=list->native_binding_device;
                        entry.binding_origin=entry.resource_origin=Provenance::ObservedMetadata;
                        auto existing=std::find_if(candidate.resources.begin(),candidate.resources.begin()+candidate.resource_count,
                            [&](const ResourceRecord& r){return r.token.id==v.resource.id;});
                        if(existing==candidate.resources.begin()+candidate.resource_count) {
                            if(candidate.resource_count>=limits.resources) {failure="ResourceBounds";break;}
                            candidate.resources[candidate.resource_count++]={v.resource,v.resource_bytes,list->native_binding_device,
                                Provenance::ObservedMetadata,Provenance::ObservedMetadata,Provenance::ObservedMetadata};
                        } else if(existing->token!=v.resource || existing->bytes!=v.resource_bytes ||
                                  existing->device!=list->native_binding_device) {
                            failure="ResourceConflict";break;
                        }
                    }
                    candidate.entries[candidate.entry_count++]=std::move(entry);
                }
                if(!failure && (s.bindingEvents.load() || s.rootCreates || s.metadataReleases ||
                    !s.nativeBindings.enabled() || b.generation!=expected.generation ||
                    b.revision!=expected.revision || b.tracking_gap || list->observation_gap))
                    failure="ChangedDuringExport";
            }
        }
    } catch(...) {failure="AllocationOrObservationFailure";}
    if(exportReentered)failure="BusyReentry";
    exportReentered=false;
    exportActive=false;
    if(failure)return refused(failure); // No accepted partial entries escape.
    candidate.outcome=Outcome::MetadataObserved;candidate.reason="MetadataObserved";
    return candidate;
}
} // namespace native_evidence
namespace {
// SDK 10.0.26100.0/um/d3d12.h C vtables: base 6593, slots 6651-6987;
// List4 SetPipelineState1 slot75 at 21818, List10 SetProgram slot84 at 35143.
// Those setters invalidate unsupported pipeline state rather than certify it.
void STDMETHODCALLTYPE setTopology(ID3D12GraphicsCommandList* self,D3D12_PRIMITIVE_TOPOLOGY topology) {
    HookAdmission admission;if(!admission)return;
    ConstantCall constantCall(self);
    using Fn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,D3D12_PRIMITIVE_TOPOLOGY);
    original<Fn>(self,20)(self,topology);
    constantCall.returned();
    observeBinding(self,[&]{auto& s=state();std::lock_guard lock(s.mutex);
        auto& b=s.states[listKey_locked(self)].bindings;++b.revision;b.topology=static_cast<uint32_t>(topology);});
}
// review 2026-11-01 F2: Dispatch is hooked only to COUNT the compute work the game records after an atlas group (gpu_copy_step5_hooks.inl
// fxAtlasComputeRecorded: relaxed atomics, no lock, no state); the call is forwarded unchanged.
void STDMETHODCALLTYPE dispatchCensus(ID3D12GraphicsCommandList* self,UINT x,UINT y,UINT z) {
    HookAdmission admission;if(!admission)return;
    fxAtlasComputeRecorded(self,false);
    using Fn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,UINT,UINT);
    original<Fn>(self,14)(self,x,y,z);
}
void STDMETHODCALLTYPE setPipeline1(ID3D12GraphicsCommandList4* self,ID3D12StateObject* object) {
    HookAdmission admission;if(!admission)return;
    ConstantCall constantCall(self);
    using Fn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*,ID3D12StateObject*);
    original<Fn>(self,75)(self,object);
    constantCall.returned();
    observeBinding(reinterpret_cast<ID3D12GraphicsCommandList*>(self),[&]{auto& s=state();std::lock_guard lock(s.mutex);
        auto& list=s.states[listKey_locked(self)];++list.bindings.revision;list.bindings.tracking_complete=false;
        if(!list.bindings.tracking_gap)list.bindings.tracking_gap="rejected:unsupported SetPipelineState1";});
}
void STDMETHODCALLTYPE setProgram(ID3D12GraphicsCommandList10* self,const D3D12_SET_PROGRAM_DESC* desc) {
    HookAdmission admission;if(!admission)return;
    ConstantCall constantCall(self);
    using Fn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList10*,const D3D12_SET_PROGRAM_DESC*);
    original<Fn>(self,84)(self,desc);
    constantCall.returned();
    observeBinding(reinterpret_cast<ID3D12GraphicsCommandList*>(self),[&]{auto& s=state();std::lock_guard lock(s.mutex);
        auto& list=s.states[listKey_locked(self)];++list.bindings.revision;list.bindings.tracking_complete=false;
        if(!list.bindings.tracking_gap)list.bindings.tracking_gap="rejected:unsupported SetProgram";});
}
HRESULT STDMETHODCALLTYPE l3Close(ID3D12GraphicsCommandList* self) {
    HookAdmission admission;if(!admission)return E_FAIL;
    CopyNativeCall copyObservation(self);
    using Fn=HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
    // J-3b7f: a whole set whose last member was not reached gets its copies
    // recorded here, still inside the open recording (no-op while OFF).
    if(g_step3SetActive.load(std::memory_order_relaxed))observeSafely([&]{step3SetClose(self);});
    // J-hud-colour (gpu_copy_step5_png_once): an armed sample's readback copies are appended to the
    // producer's frame recording here, still inside it (false unless the key armed a sample).
    if(g_step5PngWanted.load(std::memory_order_relaxed))observeSafely([&]{step5PngClose(self);});
    const auto hr=original<Fn>(self,9)(self);
    if(copyObservation.feed)copyObservation.feed->close_result(SUCCEEDED(hr));
    if(const int phase=g_step1Phase.load(std::memory_order_relaxed);phase==static_cast<int>(Step1Phase::inserted))
        observeSafely([&]{step1NoteClose(self,hr);});
    else if(phase==static_cast<int>(Step1Phase::continuous))observeSafely([&]{step5NoteClose(self,hr);});
    auto& s=state();std::lock_guard lock(s.mutex);auto it=s.states.find(listKey_locked(self));
    if(it!=s.states.end())if(auto* l=l3_list_locked(it->second)) {
        diagnostics::L3Observation::inc(l->closes);l->closed=SUCCEEDED(hr);if(FAILED(hr))l->gap=true;
    }
    return hr;
}
void STDMETHODCALLTYPE l3CopyBuffer(ID3D12GraphicsCommandList* self,ID3D12Resource* dest,UINT64 offset,ID3D12Resource* source,UINT64 sourceOffset,UINT64 bytes) {
    HookAdmission admission;if(!admission)return;
    using Fn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12Resource*,UINT64,ID3D12Resource*,UINT64,UINT64);
    // Inventory at native call entry; no lock is held across forwarding.
    {auto& s=state();std::lock_guard lock(s.mutex);auto it=s.states.find(listKey_locked(self));
     if(it!=s.states.end())if(auto* l=l3_list_locked(it->second))s.l3.copy(it->second.bindings.list_identity,l->generation,reinterpret_cast<uintptr_t>(dest));}
    original<Fn>(self,15)(self,dest,offset,source,sourceOffset,bytes);
}
void STDMETHODCALLTYPE l3CopyResource(ID3D12GraphicsCommandList* self,ID3D12Resource* dest,ID3D12Resource* source) {
    HookAdmission admission;if(!admission)return;
    using Fn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12Resource*,ID3D12Resource*);
    {auto& s=state();std::lock_guard lock(s.mutex);auto it=s.states.find(listKey_locked(self));
     if(it!=s.states.end())if(auto* l=l3_list_locked(it->second))s.l3.copy(it->second.bindings.list_identity,l->generation,reinterpret_cast<uintptr_t>(dest));}
    original<Fn>(self,17)(self,dest,source);
}
void step5NoteBarrier_locked(ListState& ls,const D3D12_RESOURCE_BARRIER& b) noexcept; // J-3b9 (below)
void STDMETHODCALLTYPE l3Barriers(ID3D12GraphicsCommandList* self,UINT count,const D3D12_RESOURCE_BARRIER* barriers) {
    HookAdmission admission;if(!admission)return;
    using Fn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,const D3D12_RESOURCE_BARRIER*);
    {auto& s=state();std::lock_guard lock(s.mutex);auto it=s.states.find(listKey_locked(self));
     if(it!=s.states.end()) {
         if(barriers)for(UINT i=0;i<count;++i){hudObserveBarrier_locked(it->first,it->second,barriers[i]);hudReplayBarrier_locked(it->first,it->second,barriers[i]);
             step5NoteBarrier_locked(it->second,barriers[i]);} // J-3b9 world RT facts; J-hud-capture region end
         else if(count){it->second.step3.rtBad=true;it->second.step3.rtWhy="barrier-array-null";}
     }
     if(it!=s.states.end())if(auto* l=l3_list_locked(it->second)) {
         if(barriers)for(UINT i=0;i<count;++i)s.l3.barrier(*l,it->second.bindings.list_identity,l->generation,barriers[i]);
         else if(count)l->gap=true;
     }}
    // J-scene-end (default off: one relaxed load): a transition of the scene target out of RENDER_TARGET ended the HUD region; copy BEFORE the game's barrier.
    if(g_step5SceneEndWanted.load(std::memory_order_relaxed))observeSafely([&]{step5SceneEnd(self,"scene-barrier");});
    original<Fn>(self,26)(self,count,barriers);
}
void l3InstallList_locked(void** table,Hooks& hooks) {
    auto& o=l3_locked();if(!o.enabled)return;
    // Optional seams never participate in the capture certification result.
    for(const auto entry:{std::pair<int,void*>{9,reinterpret_cast<void*>(&l3Close)},
            {15,reinterpret_cast<void*>(&l3CopyBuffer)},{17,reinterpret_cast<void*>(&l3CopyResource)},
            {26,reinterpret_cast<void*>(&l3Barriers)}})
        if(!patch(table,entry.first,entry.second,hooks))diagnostics::L3Observation::inc(o.patch_gaps);
}
// J-3b7 step-3 RS/OM tracking hooks: forward once, then record the arguments
// into the list's Step3Track (observation only; never alter the call).
void STDMETHODCALLTYPE step3SetViewports(ID3D12GraphicsCommandList* self,UINT count,const D3D12_VIEWPORT* viewports) {
    HookAdmission admission;if(!admission)return;
    using Fn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,const D3D12_VIEWPORT*);
    original<Fn>(self,21)(self,count,viewports);
    auto& s=state();std::lock_guard lock(s.mutex);auto it=s.states.find(listKey_locked(self));
    if(it==s.states.end())return;
    auto& t=it->second.step3;
    if(count>D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE||(count&&!viewports)){t.gap=true;return;}
    t.viewports=count;t.right=t.bottom=0;t.left=t.top=0;t.vp0={};
    for(UINT i=0;i<count;++i) {
        const auto& v=viewports[i];if(!i){t.vp0=v;t.left=v.TopLeftX;t.top=v.TopLeftY;}
        t.right=(std::max)(t.right,v.TopLeftX+v.Width);t.bottom=(std::max)(t.bottom,v.TopLeftY+v.Height);
        t.left=(std::min)(t.left,v.TopLeftX);t.top=(std::min)(t.top,v.TopLeftY);
    }
}
void STDMETHODCALLTYPE step3SetScissors(ID3D12GraphicsCommandList* self,UINT count,const D3D12_RECT* rects) {
    HookAdmission admission;if(!admission)return;
    using Fn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,const D3D12_RECT*);
    original<Fn>(self,22)(self,count,rects);
    auto& s=state();std::lock_guard lock(s.mutex);auto it=s.states.find(listKey_locked(self));
    if(it==s.states.end())return;
    auto& t=it->second.step3;
    if(count>D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE||(count&&!rects)){t.gap=true;return;}
    t.scissors=count;t.sc0=count?rects[0]:D3D12_RECT{};
}
// (the recording half of the OMSetRenderTargets hook: after the native call; split out so the J-scene-end copy runs after the hook-state lock is dropped)
static void step3SetRenderTargetsRecord(ID3D12GraphicsCommandList* self,UINT count,const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs,
                                        BOOL single,const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
    auto& s=state();std::lock_guard lock(s.mutex);auto it=s.states.find(listKey_locked(self));
    if(it==s.states.end())return;
    auto& t=it->second.step3;
    if(count>D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT||(count&&!rtvs)){t.gap=true;return;}
    t.omSet=true;t.omCount=count;t.omSingle=single;t.rtv={};
    // Single-handle form: the runtime derives the other handles from the base;
    // the restore re-issues the same base with the same flag.
    const UINT stored=single?(count?1u:0u):count;
    for(UINT i=0;i<stored;++i)t.rtv[i]=rtvs[i];
    t.dsv=dsv!=nullptr;t.dsvHandle=dsv?*dsv:D3D12_CPU_DESCRIPTOR_HANDLE{};
    // J-3b9: bind-time descriptor evidence (the single-handle form derives handles the
    // proxy cannot attribute: unproved).
    t.copyEpoch=s.step3CopyEpoch;t.omProof=!(single&&count>1);t.rtvSerial={};t.dsvSerial=0;
    auto serialOf=[&](SIZE_T h,bool depth)->uint64_t{for(const auto& v:s.step3Views)if(v.handle&&v.handle==h&&v.depth==depth)return v.serial;return 0;};
    for(UINT i=0;i<stored&&t.omProof;++i)if(!(t.rtvSerial[i]=serialOf(t.rtv[i].ptr,false)))t.omProof=false;
    if(t.dsv&&t.omProof&&!(t.dsvSerial=serialOf(t.dsvHandle.ptr,true)))t.omProof=false;
    hudObserveTarget_locked(it->first,it->second);
    hudReplayTarget_locked(it->first,it->second); // J-hud-capture region end (default OFF)
    // J-3b9c: copy the CONTENTS now (native CopyDescriptorsSimple, no hook) into the next ring
    // slot. Only descriptors the view hooks observed being written are copied (known source
    // heap/descriptor); the single-handle multi-RT form and unknown handles are not copied
    // (the restore is then refused). Cost: <= 9 CPU descriptor copies per OMSetRenderTargets,
    // only while step 3/5 is configured.
    t.privOk=false;auto& r=s.step3Om;
    if(r.ready&&t.omProof&&r.copy) {
        using CopyFn=void(STDMETHODCALLTYPE*)(ID3D12Device*,UINT,D3D12_CPU_DESCRIPTOR_HANDLE,D3D12_CPU_DESCRIPTOR_HANDLE,D3D12_DESCRIPTOR_HEAP_TYPE);
        const auto copy=reinterpret_cast<CopyFn>(r.copy);
        // A recording Draw owns its restore entry until native OM has consumed it.
        // Skip reserved entries even if another recording thread wraps repeatedly.
        uint64_t seq=0;SIZE_T slot=0;
        for(unsigned tries=0;tries<HookState::Step3OmRing::binds;++tries) {
            const auto next=++r.seq;const auto candidate=SIZE_T(next%HookState::Step3OmRing::binds);
            if(!r.pins[candidate]){seq=next;slot=candidate;break;}
        }
        if(!seq)return; // all private entries in use: fail closed
        r.versions[slot]=seq;
        for(UINT i=0;i<stored;++i){const D3D12_CPU_DESCRIPTOR_HANDLE d{r.rtvBase+(slot*8+i)*r.rtvInc};copy(r.device,1,d,t.rtv[i],D3D12_DESCRIPTOR_HEAP_TYPE_RTV);t.privRtv[i]=d;}
        if(t.dsv){const D3D12_CPU_DESCRIPTOR_HANDLE d{r.dsvBase+slot*r.dsvInc};copy(r.device,1,d,t.dsvHandle,D3D12_DESCRIPTOR_HEAP_TYPE_DSV);t.privDsv=d;}
        t.privSeq=seq;t.privOk=true;
    }
}
void STDMETHODCALLTYPE step3SetRenderTargets(ID3D12GraphicsCommandList* self,UINT count,const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs,
                                             BOOL single,const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
    HookAdmission admission;if(!admission)return;
    using Fn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,const D3D12_CPU_DESCRIPTOR_HANDLE*,BOOL,const D3D12_CPU_DESCRIPTOR_HANDLE*);
    original<Fn>(self,46)(self,count,rtvs,single,dsv);
    step3SetRenderTargetsRecord(self,count,rtvs,single,dsv);
    // J-scene-end (default off: one relaxed load): this OMSetRenderTargets may have ended the HUD region; the scene target is still RENDER_TARGET.
    if(g_step5SceneEndWanted.load(std::memory_order_relaxed))observeSafely([&]{step5SceneEnd(self,"rt-change");});
}
// J-3b9 enhanced barriers (ID3D12GraphicsCommandList7::Barrier, slot 80): not
// decoded; any call marks the generation as having untracked barriers (world refused).
void STDMETHODCALLTYPE step5EnhancedBarrier(ID3D12GraphicsCommandList* self,UINT32 groups,const void* barrierGroups) {
    HookAdmission admission;if(!admission)return;
    using Fn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT32,const void*);
    {auto& s=state();std::lock_guard lock(s.mutex);auto it=s.states.find(listKey_locked(self));
     if(it!=s.states.end())it->second.step3.enhanced=true;}
    original<Fn>(self,80)(self,groups,barrierGroups);
}
// l3Barriers seam, hook-state mutex held: legacy barriers touching the watched RT.
void step5NoteBarrier_locked(ListState& ls,const D3D12_RESOURCE_BARRIER& b) noexcept {
    const uintptr_t watch=g_step5WorldWatch.load(std::memory_order_relaxed);if(!watch)return;
    auto& s=state();auto& t=ls.step3;
    const uint64_t serial=step5WatchSerial_locked(); // J-155 fix: the watched texture's incarnation (not L3)
    const auto* recording=l3_list_locked(ls);const uint64_t generation=recording?recording->generation:0;
    if(t.rtGeneration!=generation||t.rtResource!=watch||t.rtWatchEpoch!=s.step5WatchEpoch||t.rtSerial!=serial) {
        if(t.rtPendingCounted&&s.step5RtPending>0)--s.step5RtPending;
        t.rtTransition=t.rtBad=t.rtPendingCounted=false;t.rtAfter=0;t.rtWhy=nullptr;
        t.rtResource=watch;t.rtWatchEpoch=s.step5WatchEpoch;t.rtSerial=serial;t.rtGeneration=generation;
    }
    const bool touches=(b.Type==D3D12_RESOURCE_BARRIER_TYPE_TRANSITION&&reinterpret_cast<uintptr_t>(b.Transition.pResource)==watch)||
        (b.Type==D3D12_RESOURCE_BARRIER_TYPE_UAV&&reinterpret_cast<uintptr_t>(b.UAV.pResource)==watch)||
        (b.Type==D3D12_RESOURCE_BARRIER_TYPE_ALIASING&&(!b.Aliasing.pResourceBefore||!b.Aliasing.pResourceAfter||
            reinterpret_cast<uintptr_t>(b.Aliasing.pResourceBefore)==watch||reinterpret_cast<uintptr_t>(b.Aliasing.pResourceAfter)==watch));
    if(touches&&!t.rtPendingCounted){t.rtPendingCounted=true;++state().step5RtPending;}
    if(b.Type==D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
        if(reinterpret_cast<uintptr_t>(b.Transition.pResource)!=watch)return;
        if(b.Flags&(D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY|D3D12_RESOURCE_BARRIER_FLAG_END_ONLY)){t.rtBad=true;t.rtWhy="split-barrier";return;}
        if(b.Transition.Subresource!=0&&b.Transition.Subresource!=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES){t.rtBad=true;t.rtWhy="barrier-subresource-not-all-or-0";return;}
        t.rtTransition=true;t.rtAfter=uint32_t(b.Transition.StateAfter);
    } else if(b.Type==D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
        if(!b.Aliasing.pResourceBefore||!b.Aliasing.pResourceAfter||reinterpret_cast<uintptr_t>(b.Aliasing.pResourceBefore)==watch||
           reinterpret_cast<uintptr_t>(b.Aliasing.pResourceAfter)==watch){t.rtBad=true;t.rtWhy="aliasing-barrier";}
    } else if(b.Type==D3D12_RESOURCE_BARRIER_TYPE_UAV) {
        if(reinterpret_cast<uintptr_t>(b.UAV.pResource)==watch){t.rtBad=true;t.rtWhy="uav-barrier";}
    }
}

bool installCaptureTable_locked(void** table,unsigned version) {
    if(!installListTable_locked(table))return false;
    // J-3b7: RS/OM tracking only while the step-3 experiment is configured.
    if(step3Mode()) {
        auto& step3Hooks=state().lists.at(table);
        if(!patch(table,21,reinterpret_cast<void*>(&step3SetViewports),step3Hooks)||
           !patch(table,22,reinterpret_cast<void*>(&step3SetScissors),step3Hooks)||
           !patch(table,46,reinterpret_cast<void*>(&step3SetRenderTargets),step3Hooks))return false;
        // J-3b9: enhanced barriers are only observed (not decoded); a failed patch means untracked.
        if(version>=7&&!patch(table,80,reinterpret_cast<void*>(&step5EnhancedBarrier),step3Hooks))g_step5EnhancedUntracked=true;
        // review 2026-11-01 F2: the read-only Dispatch census (a failed patch only leaves the count unknown: hook=0 on fx-puff-atlas-compute)
        g_fxAtlasDispatchHook.store(patch(table,14,reinterpret_cast<void*>(&dispatchCensus),step3Hooks)?1:0,std::memory_order_relaxed);
    }
    auto& hooks=state().lists.at(table);
    l3InstallList_locked(table,hooks);
    const std::pair<int,void*> setters[]={
        {20,reinterpret_cast<void*>(&setTopology)},
        {27,reinterpret_cast<void*>(&executeBundle)},{28,reinterpret_cast<void*>(&setHeaps)},
        {30,reinterpret_cast<void*>(&setRoot)},{32,reinterpret_cast<void*>(&setTable)},
        {34,reinterpret_cast<void*>(&setConstant)},{36,reinterpret_cast<void*>(&setConstants)},
        {38,reinterpret_cast<void*>(&setAddress<38,metadata::ParameterKind::cbv>)},
        {40,reinterpret_cast<void*>(&setAddress<40,metadata::ParameterKind::srv>)},
        {42,reinterpret_cast<void*>(&setAddress<42,metadata::ParameterKind::uav>)},
        {43,reinterpret_cast<void*>(&setIndex)},{44,reinterpret_cast<void*>(&setVertices)},
        {59,reinterpret_cast<void*>(&executeIndirect)}};
    for(auto entry:setters)if(!patch(table,entry.first,entry.second,hooks))return false;
    if(version>=4&&!patch(table,75,reinterpret_cast<void*>(&setPipeline1),hooks))return false;
    if(version>=10&&!patch(table,84,reinterpret_cast<void*>(&setProgram),hooks))return false;
    return copyInstallTable_locked(table,version,hooks);
}
constexpr IID listIids[]={__uuidof(ID3D12GraphicsCommandList),__uuidof(ID3D12GraphicsCommandList1),
    __uuidof(ID3D12GraphicsCommandList2),__uuidof(ID3D12GraphicsCommandList3),__uuidof(ID3D12GraphicsCommandList4),
    __uuidof(ID3D12GraphicsCommandList5),__uuidof(ID3D12GraphicsCommandList6),__uuidof(ID3D12GraphicsCommandList7),
    __uuidof(ID3D12GraphicsCommandList8),__uuidof(ID3D12GraphicsCommandList9),__uuidof(ID3D12GraphicsCommandList10)};
// Fixed temporary COM ownership. Destruction happens outside every hook lock,
// including on exceptions/partial installation. Never retained across lifetimes.
constexpr IID listBaseIids[]={__uuidof(ID3D12CommandList),__uuidof(ID3D12DeviceChild),__uuidof(ID3D12Object)};
struct ListInterfaces {
    std::array<IUnknown*,11> variants{};std::array<IUnknown*,3> bases{};
    IUnknown* canonical=nullptr;ID3D12Device* device=nullptr;
    // Every pointer here is an owned reference from this call's own QueryInterface/GetDevice. Section 8 (D-358): no certification
    // cache; interface addresses are never retained across calls (tear-offs may be independently reference-counted).
    ~ListInterfaces(){for(auto* p:variants)if(p)p->Release();for(auto* p:bases)if(p)p->Release();
        if(canonical)canonical->Release();if(device)device->Release();}
};
// Caller holds the capture mutex; only registry lookups and opaque equality.
// A category bit records map membership, not proof that its owner is correct.
// No COM calls, raw addresses, allocations or extra certification permissions.
void canonicalTableDiagnostic_locked(ID3D12GraphicsCommandList* self,
    const ListInterfaces& observed,diagnostics::ListSeam seam) noexcept {
    auto& s=state();const auto index=static_cast<unsigned>(seam);
    if(index>=s.canonicalTableReported.size()||s.canonicalTableReported[index])return;
    auto* log=diagnostics::sink();if(!log||!log->enabled())return;
    auto table=vtable(observed.canonical);
    const unsigned categories=(s.compatibility.count(table)?1u:0u)|(s.devices.count(table)?2u:0u)|
        (s.objects.count(table)?4u:0u)|(s.queues.count(table)?8u:0u)|(s.lists.count(table)?16u:0u);
    unsigned equal=(observed.canonical==reinterpret_cast<IUnknown*>(self)?1u:0u)|
        (table==vtable(self)?8u:0u);
    for(auto* p:observed.variants)if(p) {
        if(p==observed.canonical)equal|=2u;
        if(vtable(p)==table)equal|=16u;
    }
    for(auto* p:observed.bases)if(p) {
        if(p==observed.canonical)equal|=4u;
        if(vtable(p)==table)equal|=32u;
    }
    s.canonicalTableReported[index]=true;
    diagnostics::emit(diagnostics::record(diagnostics::Event::device,categories,equal,index,
        "list canonical table categories;a=compatibility:1,devices:2,objects:4,queues:8,lists:16;"
        "b=pointer-self:1,variant:2,ancestor:4,table-self:8,variant:16,ancestor:32;"
        "c=creation:0,Reset:1,ExecuteCommandLists:2;first-example"));
    // The existing startup cap can drop this supplemental example. The
    // separately reserved reason/seam refusal counter remains authoritative.
}

}
bool hookCaptureCommandList(ID3D12GraphicsCommandList* list,diagnostics::ListSeam seam) {
    HookAdmission admission;if(!admission)return false;
    using Reason=diagnostics::ListRefusal;
    struct FailureGuard {
        ID3D12GraphicsCommandList* self;diagnostics::ListSeam seam;
        Reason reason=Reason::exception;unsigned type=UINT_MAX;bool success=false;
        bool refuse(Reason why) noexcept {reason=why;return false;}
        ~FailureGuard() noexcept {
            if(success)return;
            try {
                ListStates::node_type retired;
                auto& s=state();std::lock_guard lock(s.mutex);
                diagnostics::list_refused(reason,seam,type);
                if(!self)return;
                auto key=listKey_locked(self);
                auto found=s.states.find(key);
                if(found!=s.states.end()&&found->second.copyObservation) {
                    found->second.copyObservation->installed(0);
                    found->second.copyObservation->uncertain("list-certification-failed");
                }
                // Retain only per-object published routing, never infer it
                // from tables patched by another object. Early refusals have
                // only the bounded reason/seam record until routing exists.
                auto covered=[](void** table) {return releaseCovered_locked(table);};
                const auto alias=s.listKeys.find(self);
                bool lifetime=found!=s.states.end()&&found->second.cleanup_established&&
                    alias!=s.listKeys.end()&&alias->second==key&&covered(vtable(self));
                if(found!=s.states.end())for(auto table:found->second.tables)
                    if(table&&!covered(table))lifetime=false;
                if(!lifetime) {
                    if(found!=s.states.end()) {
                        retired=s.states.extract(found);
                    }
                    for(auto alias=s.listKeys.begin();alias!=s.listKeys.end();)
                        if(alias->second==key)alias=s.listKeys.erase(alias);else ++alias;
                    return; // retired COM owners are released after the mutex.
                }
                auto& point=found->second;
                point.capture_hooks=false;point.interfaces_observed=false;
                point.bindings.tracking_complete=false;point.bindings.list_identity=0;
                point.observation_gap=diagnostics::list_refusal_reasons[static_cast<unsigned>(reason)];
            } catch(...) {} // No optional diagnostic may throw during unwinding.
        }
    } failure{list,seam};
    try {
    if(!list)return failure.refuse(Reason::null_list);
    if(!pinProxy())return failure.refuse(Reason::pin);
    ListInterfaces observed;
    // Queries and native GetDevice happen before locking; certification uses
    // canonical equality, never the interface address or file offsets as identity.
    if(FAILED(list->QueryInterface(IID_PPV_ARGS(&observed.canonical)))||!observed.canonical)return failure.refuse(Reason::canonical_qi);
    failure.type=static_cast<unsigned>(list->GetType());
    if(failure.type!=D3D12_COMMAND_LIST_TYPE_DIRECT)return failure.refuse(
        failure.type==D3D12_COMMAND_LIST_TYPE_BUNDLE?Reason::non_direct_bundle:Reason::non_direct_other);
    for(unsigned i=0;i<std::size(listIids);++i) {
        const auto hr=list->QueryInterface(listIids[i],reinterpret_cast<void**>(&observed.variants[i]));
        if(hr==E_NOINTERFACE) {if(observed.variants[i])return failure.refuse(Reason::variant_unexpected);continue;}
        if(FAILED(hr)||!observed.variants[i])return failure.refuse(Reason::variant_qi);
        IUnknown* identity=nullptr;
        const auto qi=observed.variants[i]->QueryInterface(IID_PPV_ARGS(&identity));
        std::unique_ptr<IUnknown,ReleaseCOM> owned(identity);
        if(FAILED(qi)||identity!=observed.canonical)return failure.refuse(Reason::variant_canonical);
    }
    // Ancestor interfaces have no graphics setters, but final Release may
    // arrive through a separate ancestor table. Track that lifetime seam too.
    for(unsigned i=0;i<std::size(listBaseIids);++i) {
        const auto hr=list->QueryInterface(listBaseIids[i],reinterpret_cast<void**>(&observed.bases[i]));
        if(hr==E_NOINTERFACE){if(observed.bases[i])return failure.refuse(Reason::ancestor_unexpected);continue;}
        if(FAILED(hr)||!observed.bases[i])return failure.refuse(Reason::ancestor_qi);
        IUnknown* identity=nullptr;
        const auto qi=observed.bases[i]->QueryInterface(IID_PPV_ARGS(&identity));
        std::unique_ptr<IUnknown,ReleaseCOM> owned(identity);
        if(FAILED(qi)||identity!=observed.canonical)return failure.refuse(Reason::ancestor_canonical);
    }
    if(!observed.variants[0])return failure.refuse(Reason::base_variant_missing);
    IUnknown* stable=nullptr;
    auto stableHr=observed.canonical->QueryInterface(IID_PPV_ARGS(&stable));
    std::unique_ptr<IUnknown,ReleaseCOM> stableOwned(stable);
    if(FAILED(stableHr)||stable!=observed.canonical)return failure.refuse(Reason::stable_qi);
    ID3D12GraphicsCommandList* roundTrip=nullptr;
    auto roundHr=observed.canonical->QueryInterface(IID_PPV_ARGS(&roundTrip));
    std::unique_ptr<ID3D12GraphicsCommandList,ReleaseCOM> roundOwned(roundTrip);
    if(FAILED(roundHr)||roundTrip!=observed.variants[0])return failure.refuse(Reason::roundtrip_qi);
    bool returnedSelf=false;for(auto* p:observed.variants)returnedSelf|=p==list;
    if(!returnedSelf)return failure.refuse(Reason::self_missing);
    const auto deviceHr=list->GetDevice(IID_PPV_ARGS(&observed.device));
    // Shared patched tables can observe raw aliases before late discovery.
    // Discard those unregistered histories; never merge recorded bindings or
    // counters. Their COM owners are destroyed after releasing the mutex.
    // Detached nodes retain their existing storage and COM owners until after
    // unlocking. Local handles remain independent across nested native calls.
    std::array<ListStates::node_type,14> displaced;
    auto& s=state();std::lock_guard lock(s.mutex);
    auto key=listKey_locked(list);
    // Already-certified conflicting identities are never combined.
    auto compatible=[&](void* alias) {
        auto it=s.listKeys.find(alias);
        return (it==s.listKeys.end()||it->second==key)&&
            (alias==key||!s.states.count(static_cast<ID3D12GraphicsCommandList*>(alias))||
             (it==s.listKeys.end()&&!s.states.at(static_cast<ID3D12GraphicsCommandList*>(alias)).interfaces_observed));
    };
    auto metadataOwner=[&](IUnknown* p) {
        const auto address=reinterpret_cast<uintptr_t>(p);
        return s.rootKeys.count(p)||s.rootIdentities.count(reinterpret_cast<ID3D12RootSignature*>(p))||
            pipelineFor_locked(p)!=s.pipelines.end()||
            s.nativeBindings.heap_token(address).id||s.nativeBindings.resource_token(address).id;
    };
    if(metadataOwner(observed.canonical))return failure.refuse(Reason::canonical_alias);
    for(auto* p:observed.variants)if(p&&metadataOwner(p))return failure.refuse(Reason::variant_alias);
    for(auto* p:observed.bases)if(p&&metadataOwner(p))return failure.refuse(Reason::ancestor_alias);
    if(!compatible(observed.canonical))return failure.refuse(Reason::canonical_alias);
    for(auto* p:observed.variants)if(p&&!compatible(p))return failure.refuse(Reason::variant_alias);
    for(auto* p:observed.bases)if(p&&!compatible(p))return failure.refuse(Reason::ancestor_alias);
    unsigned discarded=0;
    for(auto* variant:observed.variants)if(variant&&variant!=key) {
        auto it=s.states.find(reinterpret_cast<ID3D12GraphicsCommandList*>(variant));
        if(it!=s.states.end()) {
            if(it->second.copyObservation)it->second.copyObservation->uncertain("list-alias-displaced");
            displaced[discarded++]=s.states.extract(it);
        }
    }
    for(auto* ancestor:observed.bases)if(ancestor&&ancestor!=key) {
        auto it=s.states.find(reinterpret_cast<ID3D12GraphicsCommandList*>(ancestor));
        if(it!=s.states.end()) {
            if(it->second.copyObservation)it->second.copyObservation->uncertain("list-alias-displaced");
            displaced[discarded++]=s.states.extract(it);
        }
    }
    auto& current=s.states[key];current.table=vtable(list);
    current.cleanup_established=false;
    if(discarded)current.bindings.list_identity=capture::new_lifetime_identity();
    current.tables[0]=vtable(observed.canonical);
    for(unsigned i=0;i<observed.variants.size();++i)
        current.tables[i+1]=observed.variants[i]?vtable(observed.variants[i]):nullptr;
    for(unsigned i=0;i<observed.bases.size();++i)
        current.tables[i+12]=observed.bases[i]?vtable(observed.bases[i]):nullptr;
    // Invalidate before any allocating registration or patch. An unsuccessful
    // install cannot leave an older recording eligible. Successful retries also
    // require a fresh Reset; already-complete reinstalls preserve their state.
    const auto prior=current.capture_hooks&&!discarded;current.capture_hooks=false;
    try {
        s.listKeys[list]=key;s.listKeys[observed.canonical]=key;
        for(auto* p:observed.variants)if(p)s.listKeys[p]=key;
        for(auto* p:observed.bases)if(p)s.listKeys[p]=key;
        // Patch failures use nonallocating tagged throws; arbitrary exceptions
        // retain the default exception reason. Catch invalidates shared tables.
        auto lifetime=[&](void** table,Reason unavailable,Reason patchFailed) {
            // Only lifetime-only interfaces may borrow metadata Release. All
            // graphics/setter category exclusions remain unchanged below.
            auto metadata=s.objects.find(table);
            if(!s.compatibility.count(table)&&!s.devices.count(table)&&!s.queues.count(table)&&
               metadata!=s.objects.end()&&metadata->second.metadataRelease) {
                auto* owner=releaseHooks_locked(table);
                if(owner) {
                    const auto handler=metadata->second.originals[2]?
                        reinterpret_cast<void*>(&releaseMetadata):reinterpret_cast<void*>(&hookedRelease);
                    if(patchRelease_locked(table,handler,*owner))return;
                    failure.reason=patchFailed;throw patchFailed;
                }
            }
            if(!listTableAvailable_locked(table)){
                if(unavailable==Reason::canonical_table)canonicalTableDiagnostic_locked(list,observed,seam);
                failure.reason=unavailable;throw unavailable;
            }
            if(!patchRelease_locked(table,reinterpret_cast<void*>(&hookedRelease),s.lists[table])) {
                failure.reason=patchFailed;throw patchFailed;
            }
        };
        lifetime(vtable(observed.canonical),Reason::canonical_table,Reason::canonical_lifetime);
        for(auto* variant:observed.variants)if(variant)
            lifetime(vtable(variant),Reason::variant_table,Reason::variant_lifetime);
        for(auto* ancestor:observed.bases)if(ancestor)
            lifetime(vtable(ancestor),Reason::ancestor_table,Reason::ancestor_lifetime);
        current.cleanup_established=true;
        uint64_t copyAliases=0;
        for(unsigned i=0;i<observed.variants.size();++i)if(observed.variants[i]) {
            auto table=vtable(observed.variants[i]);
            if(!listTableAvailable_locked(table)){failure.reason=Reason::setter_table;throw Reason::setter_table;}
            if(!installCaptureTable_locked(table,i)){failure.reason=Reason::setter_patch;throw Reason::setter_patch;}
            copyAliases|=gpu_copy::coverage_bit(gpu_copy::Coverage::alias0)<<i;
        }
        copyAttest_locked(current,copyAliases);
        if(!current.bindings.list_identity)current.bindings.list_identity=capture::new_lifetime_identity();
        current.capture_hooks=true;current.interfaces_observed=true;
        if(SUCCEEDED(deviceHr)&&observed.device&&s.devices.count(vtable(observed.device)))
            current.native_binding_device=reinterpret_cast<uintptr_t>(observed.device);
        if(!prior) {current.bindings.tracking_complete=false;current.native_binding_history=false;
            current.reset_observed=false;current.observation_gap=nullptr;}
        failure.success=true;return true;
    } catch(...) {
        for(auto& item:s.states) {
            bool affected=item.first==key||item.second.table==current.table;
            for(auto table:current.tables)if(table) {
                affected|=item.second.table==table;
                for(auto other:item.second.tables)affected|=other==table;
            }
            if(affected) {
                if(item.second.copyObservation)item.second.copyObservation->installed(0);
                item.second.bindings.tracking_complete=false;item.second.capture_hooks=false;
                item.second.interfaces_observed=false;item.second.bindings.list_identity=0;
                item.second.observation_gap=item.first==key?
                    diagnostics::list_refusal_reasons[static_cast<unsigned>(failure.reason)]:
                    diagnostics::shared_table_invalidation;
            }
        }
        return false;
    }
    } catch(...) {throw;} // Preserve the installer's exception contract; guard records the gap.
}

// COM is required to provide nonthrowing Release. The supported exception seam
// may publish a valid, caller-owned reference before throwing, but never releases
// that published reference itself. No C++ guard can recover an arbitrary dangling
// foreign output or a Release that throws after an unknown ownership transition.
struct FactoryReference {
    void* value=nullptr;
    ~FactoryReference() {if(value) static_cast<IUnknown*>(value)->Release();}
    FactoryReference()=default;
    FactoryReference(const FactoryReference&)=delete;
    FactoryReference& operator=(const FactoryReference&)=delete;
    void* detach() noexcept {auto* p=value;value=nullptr;return p;}
};

class WrappedFactory : public IDXGIFactory6 {
public:
    WrappedFactory(IDXGIFactory6* real) : m_real(real) {}
    ~WrappedFactory() { m_real->Release(); }

    // ── IUnknown ──
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (!isWrappedIID(riid)) return E_NOINTERFACE;
        try {
            FactoryReference supported;
            HRESULT hr = m_real->QueryInterface(riid, &supported.value);
            if (FAILED(hr)) return hr;
            if (!supported.value) return E_NOINTERFACE;
            *ppv = static_cast<IDXGIFactory6*>(this);
            AddRef();
            return S_OK;
        } catch(const std::bad_alloc&) {return E_OUTOFMEMORY;}
          catch(...) {return E_FAIL;}
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG ref = --m_refs;
        if (!ref) {
            delete this; // Real factory Release returns before diagnostics I/O.
            diagnostics::flush_after_release();
            ee_camera::notify_final(); // J-w1-ee-camera rev 4 (null when OFF)
        }
        return ref;
    }

    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID n, UINT s, const void* d) override { return m_real->SetPrivateData(n,s,d); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID n, const IUnknown* u) override { return m_real->SetPrivateDataInterface(n,u); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID n, UINT* s, void* d) override { return m_real->GetPrivateData(n,s,d); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID r, void** p) override { return m_real->GetParent(r,p); }

    // ── IDXGIFactory ──
    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT i, IDXGIAdapter** a) override { return m_real->EnumAdapters(i,a); }
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND w, UINT f) override { return m_real->MakeWindowAssociation(w,f); }
    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND* w) override { return m_real->GetWindowAssociation(w); }
    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** sc) override {
        HRESULT hr = m_real->CreateSwapChain(dev, d, sc);
        if (SUCCEEDED(hr)) observeSafely([&]{captureDevice(dev);});
        if (SUCCEEDED(hr) && sc && *sc) { observeSafely([&]{noteVscreenSwapchainQueue(dev, *sc);}); ee_camera::notify_swapchain(*sc); } // J-w1-ee-camera (Present seam A); VSCREEN queue association
        return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE m, IDXGIAdapter** a) override { return m_real->CreateSoftwareAdapter(m,a); }

    // ── IDXGIFactory1 ──
    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT i, IDXGIAdapter1** a) override { return m_real->EnumAdapters1(i,a); }
    BOOL STDMETHODCALLTYPE IsCurrent() override { return m_real->IsCurrent(); }

    // ── IDXGIFactory2 ──
    BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override { return m_real->IsWindowedStereoEnabled(); }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown* dev, HWND w, const DXGI_SWAP_CHAIN_DESC1* d, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fd, IDXGIOutput* o, IDXGISwapChain1** sc) override {
        HRESULT hr = m_real->CreateSwapChainForHwnd(dev, w, d, fd, o, sc);
        if (SUCCEEDED(hr)) observeSafely([&]{captureDevice(dev);});
        if (SUCCEEDED(hr) && sc && *sc) { observeSafely([&]{noteVscreenSwapchainQueue(dev, *sc);}); ee_camera::notify_swapchain(*sc); }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown* dev, IUnknown* w, const DXGI_SWAP_CHAIN_DESC1* d, IDXGIOutput* o, IDXGISwapChain1** sc) override {
        HRESULT hr = m_real->CreateSwapChainForCoreWindow(dev, w, d, o, sc);
        if (SUCCEEDED(hr)) observeSafely([&]{captureDevice(dev);});
        if (SUCCEEDED(hr) && sc && *sc) { observeSafely([&]{noteVscreenSwapchainQueue(dev, *sc);}); ee_camera::notify_swapchain(*sc); }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE h, LUID* l) override { return m_real->GetSharedResourceAdapterLuid(h,l); }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND w, UINT m, DWORD* c) override { return m_real->RegisterStereoStatusWindow(w,m,c); }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE h, DWORD* c) override { return m_real->RegisterStereoStatusEvent(h,c); }
    void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD c) override { m_real->UnregisterStereoStatus(c); }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND w, UINT m, DWORD* c) override { return m_real->RegisterOcclusionStatusWindow(w,m,c); }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE h, DWORD* c) override { return m_real->RegisterOcclusionStatusEvent(h,c); }
    void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD c) override { m_real->UnregisterOcclusionStatus(c); }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown* dev, const DXGI_SWAP_CHAIN_DESC1* d, IDXGIOutput* o, IDXGISwapChain1** sc) override {
        HRESULT hr = m_real->CreateSwapChainForComposition(dev, d, o, sc);
        if (SUCCEEDED(hr)) observeSafely([&]{captureDevice(dev);});
        if (SUCCEEDED(hr) && sc && *sc) { observeSafely([&]{noteVscreenSwapchainQueue(dev, *sc);}); ee_camera::notify_swapchain(*sc); }
        return hr;
    }

    // ── IDXGIFactory3 ──
    UINT STDMETHODCALLTYPE GetCreationFlags() override { return m_real->GetCreationFlags(); }

    // ── IDXGIFactory4 ──
    HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID l, REFIID r, void** a) override { return m_real->EnumAdapterByLuid(l,r,a); }
    HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID r, void** a) override { return m_real->EnumWarpAdapter(r,a); }

    // ── IDXGIFactory5 ──
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(DXGI_FEATURE f, void* d, UINT s) override { return m_real->CheckFeatureSupport(f,d,s); }

    // ── IDXGIFactory6 ──
    HRESULT STDMETHODCALLTYPE EnumAdapterByGpuPreference(UINT i, DXGI_GPU_PREFERENCE p, REFIID r, void** a) override { return m_real->EnumAdapterByGpuPreference(i,p,r,a); }

private:
    IDXGIFactory6* m_real;
    std::atomic<ULONG> m_refs{1};

    void captureDevice(IUnknown* dev) { captureDeviceAndQueue(dev); }
};


using FactoryFn=HRESULT(WINAPI*)(REFIID,void**);
using Factory2Fn=HRESULT(WINAPI*)(UINT,REFIID,void**);
#ifdef ACZ_VR_OFFLINE_TEST
FactoryFn g_factoryTest=nullptr, g_factory1Test=nullptr;
Factory2Fn g_factory2Test=nullptr;
#endif
static HRESULT createWrappedFactory(unsigned version,UINT flags,REFIID iid,void** out) noexcept {
    if(out) *out=nullptr;
    try {
    FactoryFn fn=nullptr; Factory2Fn fn2=nullptr;
    const bool loaded=loadRealDXGI();
#ifndef ACZ_VR_OFFLINE_TEST
    // LAUNCH-MODE (docs/world-render/launch-mode.md): VR Off = the plain system export. No diagnostics activation,
    // no callback registration, no wrapper (so no swap-chain/device hook can ever be installed), no XR.
    if(!g_loading && !launch_mode::vr_enabled()) {
        if(!out) return E_POINTER;
        if(!loaded) return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
        if(version==2) {
            const auto real2=reinterpret_cast<Factory2Fn>(GetProcAddress(g_realDXGI,"CreateDXGIFactory2"));
            return real2?real2(flags,iid,out):HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        }
        const auto real=reinterpret_cast<FactoryFn>(GetProcAddress(g_realDXGI,version==0?"CreateDXGIFactory":"CreateDXGIFactory1"));
        return real?real(iid,out):HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    }
#endif
    // LoadLibrary can synchronously reenter an export while still loading. The
    // outer call activates after its loader guard unwinds, never from reentry.
    if(!g_loading) diagnostics::activate();
    if(!out) return E_POINTER; // Preserve pointer-error precedence even if load failed.
    if(!loaded) return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
    // J-017 owns one-time registration and terminal partial-registration policy.
    // Optional registration failure must never prevent the original export call.
    observeSafely([] {
#ifdef ACZ_VR_OFFLINE_TEST
        if(g_initializeTest) g_initializeTest();
#else
        initializeProxyCallbacks(); // outside the loader lock
#endif
    });
#ifdef ACZ_VR_OFFLINE_TEST
    fn=version==0?g_factoryTest:g_factory1Test; fn2=g_factory2Test;
#else
    fn=reinterpret_cast<FactoryFn>(GetProcAddress(g_realDXGI,version==0?"CreateDXGIFactory":"CreateDXGIFactory1"));
    fn2=reinterpret_cast<Factory2Fn>(GetProcAddress(g_realDXGI,"CreateDXGIFactory2"));
#endif
    if((version==2 && !fn2) || (version!=2 && !fn)) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    // Ask the matching real export for exactly the caller's IID/flags.
    HRESULT hr;
    try {hr=version==2?fn2(flags,iid,out):fn(iid,out);}
    catch(...) {
        // A throwing export has no native HRESULT. Clear its published valid COM
        // reference under the documented seam contract; never assume it released.
        FactoryReference exceptional;exceptional.value=*out;*out=nullptr;
        throw;
    }
    if(FAILED(hr) || !*out || !isWrappedIID(iid)) return hr;
    // Caller retains the original output until a fully queried wrapper is ready.
    // Any optional failure below preserves both native HRESULT and native output.
    try {
    auto* requested=static_cast<IUnknown*>(*out);
    FactoryReference real;
    if(FAILED(requested->QueryInterface(__uuidof(IDXGIFactory6),&real.value)) || !real.value) return hr;
    // Wrapper vtables also reside in the proxy before any D3D12 hook exists.
    if(!pinProxy()) return hr;
#ifdef ACZ_VR_OFFLINE_TEST
    if(g_allocateFactoryTest && !g_allocateFactoryTest()) return hr;
#endif
    FactoryReference wrapper;
    auto* wrapped=new(std::nothrow) WrappedFactory(static_cast<IDXGIFactory6*>(real.value));
    if(!wrapped) return hr;
    real.detach(); // Constructor accepted the Factory6 reference.
    wrapper.value=static_cast<IDXGIFactory6*>(wrapped);
    FactoryReference result;
#ifdef ACZ_VR_OFFLINE_TEST
    if(g_wrapperQueryTest) g_wrapperQueryTest(false);
#endif
    HRESULT wrappedHr=wrapped->QueryInterface(iid,&result.value);
#ifdef ACZ_VR_OFFLINE_TEST
    if(g_wrapperQueryTest) g_wrapperQueryTest(true);
#endif
    if(FAILED(wrappedHr) || !result.value) return hr;
    requested->Release(); *out=result.detach();
    } catch(...) {} // Optional wrapping failed; caller still owns original output.
    return hr;
    } catch(const std::bad_alloc&) {
        if(!g_loading) diagnostics::activate();return out?E_OUTOFMEMORY:E_POINTER;
    } catch(...) {
        if(!g_loading) diagnostics::activate();return out?E_FAIL:E_POINTER;
    }
}
} // namespace acz
extern "C" {
HRESULT WINAPI ProxyCreateDXGIFactory(REFIID iid,void** out) {return acz::createWrappedFactory(0,0,iid,out);}
HRESULT WINAPI ProxyCreateDXGIFactory1(REFIID iid,void** out) {return acz::createWrappedFactory(1,0,iid,out);}
HRESULT WINAPI ProxyCreateDXGIFactory2(UINT flags,REFIID iid,void** out) {return acz::createWrappedFactory(2,flags,iid,out);}
// J-3b4: resolved by the optional d3d12.dll proxy with GetProcAddress. Does
// nothing unless gpu_copy_early_device_hook and the step-1 opt-ins are latched.
unsigned WINAPI AczVrNotifyDeviceCreated(IUnknown* device,const GUID* iid) {return acz::notifyEarlyDevice(device,iid);}
}

// J-3b8 additive public seam (stereo_pose_provider.h): the step-5 producer's
// FrameSource and the consumer's pose provider registration.
namespace acz::stereo {
void set_pose_provider(PoseProvider* provider) noexcept {acz::step5SetPoseProvider(provider);}
FrameSource* frame_source() noexcept {return acz::step5FrameSource();}
}
